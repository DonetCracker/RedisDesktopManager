#include "abstractnamespaceitem.h"

#include <QApplication>
#include <QMessageBox>
#include <QThread>

#include "connections-tree/model.h"
#include "connections-tree/operations.h"
#include "connections-tree/keysrendering.h"
#include "keyitem.h"
#include "namespaceitem.h"
#include "loadmoreitem.h"

using namespace ConnectionsTree;

AbstractNamespaceItem::AbstractNamespaceItem(
    Model& model, QWeakPointer<TreeItem> parent,
    QSharedPointer<Operations> operations, uint dbIndex, QRegExp filter)
    : TreeItem(model),
      m_parent(parent),
      m_loaderStub(nullptr),
      m_operations(operations),
      m_filter(filter.isEmpty() ? QRegExp(operations->defaultFilter())
                                : filter),
      m_expanded(false),
      m_dbIndex(dbIndex),
      m_runningOperation(nullptr),
      m_childCountBeforeFetch(0u){
  QSettings settings;
  m_showNsOnTop = settings
                      .value("app/showNamespacesOnTop",
#if defined(Q_OS_WINDOWS)
                             true
#else
                             false
#endif
                             )
                      .toBool();
}

QList<QSharedPointer<TreeItem>> AbstractNamespaceItem::getAllChilds() const {
  return m_childItems;
}

QList<QSharedPointer<AbstractNamespaceItem>>
AbstractNamespaceItem::getAllChildNamespaces() const {
  return m_childNamespaces.values();
}

QSharedPointer<TreeItem> AbstractNamespaceItem::child(uint row) {
  if (row < m_childItems.size()) return m_childItems.at(row);
  if (row == m_childItems.size() && m_loaderStub) {
      return m_loaderStub;
  }

  return QSharedPointer<TreeItem>();
}

QWeakPointer<TreeItem> AbstractNamespaceItem::parent() const {
  return m_parent;
}

void AbstractNamespaceItem::appendRawKey(const QByteArray &k) {
    m_rawChildKeys.append(k);

    if (!m_loaderStub) {
        m_loaderStub = QSharedPointer<TreeItem>(new LoadMoreItem(getSelf(), m_model));
    }
}

bool compareNamespaces(QSharedPointer<TreeItem> first,
                       QSharedPointer<TreeItem> second) {
    if (first->type() != second->type()) return first->type() > second->type();

    return first->getDisplayName() < second->getDisplayName();
}

void AbstractNamespaceItem::appendNamespace(
    QSharedPointer<AbstractNamespaceItem> item) {
  m_childNamespaces[item->getName()] = item;
  m_childItems.append(item.staticCast<TreeItem>());

  if (m_showNsOnTop) {
    std::sort(m_childItems.begin(), m_childItems.end(), compareNamespaces);
  }
}

uint AbstractNamespaceItem::childCount(bool recursive) const {
  uint count = 0;

  if (m_loaderStub) {
      count++;
  }

  if (!recursive) {
    count += m_childItems.size();
    return count;
  }

  for (const auto &item : m_childItems) {
    if (item->supportChildItems()) {
      count += item->childCount(true);
    } else {
      count += 1;
    }
  }
  return count;
}

void AbstractNamespaceItem::clear() {
  m_model.itemChildsUnloaded(getSelf());
  m_childItems.clear();
  m_childNamespaces.clear();
  m_rawChildKeys.clear();
  m_usedMemory = 0;
}

void AbstractNamespaceItem::notifyModel() {
  qDebug() << "Notify model about loaded childs";
  m_model.itemChildsLoaded(getSelf());
  m_model.itemChanged(getSelf());
}

void AbstractNamespaceItem::showLoadingError(const QString& err) {
  m_model.itemChanged(getSelf());
  emit m_model.error(err);
}

void AbstractNamespaceItem::cancelCurrentOperation() {
  if (m_runningOperation) {
    m_runningOperation->future().cancel();
    m_operations->resetConnection();
    unlock();
  }
}

bool compareChilds(QSharedPointer<TreeItem> first,
                   QSharedPointer<TreeItem> second) {
  auto firstMemoryItem = first.dynamicCast<MemoryUsage>();
  auto secondMemoryItem = second.dynamicCast<MemoryUsage>();

  if (!firstMemoryItem)
    qDebug() << "Invalid tree item:" << first->getDisplayName();
  if (!secondMemoryItem)
    qDebug() << "Invalid tree item:" << second->getDisplayName();

  return (firstMemoryItem ? firstMemoryItem->usedMemory() : 0) >
         (secondMemoryItem ? secondMemoryItem->usedMemory() : 0);
}

void AbstractNamespaceItem::sortChilds() {
  emit m_model.beforeItemLayoutChanged(getSelf());
  std::sort(m_childItems.begin(), m_childItems.end(), compareChilds);
  emit m_model.itemLayoutChanged(getSelf());
  emit m_model.itemChanged(getSelf());
}

QHash<QString, std::function<void()>> AbstractNamespaceItem::eventHandlers() {
  auto events = TreeItem::eventHandlers();

  events.insert("analyze_memory_usage", [this]() {
    if (m_usedMemory > 0) return;

    lock();

    auto future = m_operations->connectionSupportsMemoryOperations();

    AsyncFuture::observe(future).subscribe([this](bool isSupported) {
      if (!isSupported) {
        emit m_model.error(QCoreApplication::translate(
            "RDM",
            "Your redis-server doesn't support <a "
            "href='https://redis.io/commands/memory-usage'><b>MEMORY</b></a> "
            "commands."));
        unlock();
        return;
      }

      getMemoryUsage([this](qlonglong) {
        sortChilds();
        unlock();
        m_runningOperation.clear();
      });
    });
  });

  return events;
}

void AbstractNamespaceItem::getMemoryUsage(
    std::function<void(qlonglong)> callback) {
  m_usedMemory = 0;

  m_runningOperation = QSharedPointer<AsyncFuture::Deferred<qlonglong>>(
      new AsyncFuture::Deferred<qlonglong>());

  QtConcurrent::run(this, &AbstractNamespaceItem::calculateUsedMemory,
                    m_runningOperation, callback);

  return;
}

void AbstractNamespaceItem::fetchMore() {
  if (m_rawChildKeys.size() == 0) {
    return;
  }

  lock();

  QSettings appSettings;
  const uint maxChilds = appSettings.value("app/treeItemMaxChilds", 1000).toUInt();

  int childsCount = m_childItems.size();
  auto settings = ConnectionsTree::KeysTreeRenderer::RenderingSettigns{
      m_filter, m_operations->getNamespaceSeparator(), m_dbIndex, false,
      static_cast<uint>(childsCount) + maxChilds, false
  };

  auto rawKeys = m_rawChildKeys;

  // Remove loader
  if (m_loaderStub) {
      m_model.beforeItemChildRemoved(getSelf(), childsCount);
      auto ptr = m_loaderStub;
      m_loaderStub.clear();
      m_model.itemChildRemoved(ptr.toWeakRef());
  }

  m_model.itemChanged(getSelf());
  m_childCountBeforeFetch = childsCount;
  m_rawChildKeys.clear();

  int nextChunkSize = qMin(static_cast<int>(maxChilds),
                           rawKeys.size());

  qDebug() << "Next chunck size: " << nextChunkSize;

  m_model.beforeChildLoaded(getSelf(), nextChunkSize);

  unlock();
  AsyncFuture::observe(
      QtConcurrent::run(
          &ConnectionsTree::KeysTreeRenderer::renderKeys, m_operations, rawKeys,
          qSharedPointerDynamicCast<AbstractNamespaceItem>(getSelf()), settings,
          m_model.m_expanded))
      .subscribe([this]() {
           m_model.childLoaded(getSelf());
           m_model.itemChanged(getSelf());
      });
}

uint AbstractNamespaceItem::childCountBeforeFetch()
{
    return m_childCountBeforeFetch;
}

void AbstractNamespaceItem::calculateUsedMemory(
    QSharedPointer<AsyncFuture::Deferred<qlonglong>> parentDeffered,
    std::function<void(qlonglong)> callback) {
  if (parentDeffered && parentDeffered->future().isCanceled()) {
    return;
  }

  if (m_rawChildKeys.size() > 0) {
    operations()->getUsedMemory(
        m_rawChildKeys, m_dbIndex,
        [this, callback](qlonglong result) {
          m_usedMemory = result;
          emit m_model.itemChanged(getSelf());
          callback(result);
        },
        [this](qlonglong progress) {
          m_usedMemory = progress;
          emit m_model.itemChanged(getSelf());
        });
    return;
  } else {
    auto resultsRemaining = QSharedPointer<qlonglong>(new qlonglong(0));

    auto updateUsedMemoryValue = [this, resultsRemaining,
                                  callback](qlonglong result) {
      if (!m_usedMemory) return;

      QMutexLocker locker(&m_updateUsedMemoryMutex);
      Q_UNUSED(locker);
      m_usedMemory += result;
      m_model.itemChanged(getSelf());

      (*resultsRemaining)--;

      if (*resultsRemaining <= 0) {
        callback(m_usedMemory);
      }
    };

    (*resultsRemaining) += m_childNamespaces.size();

    for (auto childNs : m_childNamespaces) {
      if (parentDeffered->future().isCanceled()) {
        return;
      }
      childNs->calculateUsedMemory(parentDeffered, updateUsedMemoryValue);
    }

    QMutexLocker locker(&m_updateUsedMemoryMutex);

    for (QSharedPointer<TreeItem> child : m_childItems) {
      if (parentDeffered->future().isCanceled()) {
        return;
      }

      if (!child || child->type() != "key") continue;

      auto memoryItem = child.dynamicCast<MemoryUsage>();

      if (!memoryItem) continue;

      (*resultsRemaining)++;
      memoryItem->getMemoryUsage(updateUsedMemoryValue);
    }
  }
}
