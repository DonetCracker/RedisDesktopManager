#include "keysrendering.h"

#include "items/abstractnamespaceitem.h"
#include "items/keyitem.h"
#include "items/namespaceitem.h"

using namespace ConnectionsTree;

QSharedPointer<AbstractNamespaceItem> ConnectionsTree::resolveRootItem(QSharedPointer<AbstractNamespaceItem> item) {
  if (!item) return QSharedPointer<AbstractNamespaceItem>();

  if (item->type() == "database") {
      return item;
  }

  auto parent = item->parent().toStrongRef();

  if (!parent) return QSharedPointer<AbstractNamespaceItem>();

  if (parent->type() == "database")
    return parent.dynamicCast<AbstractNamespaceItem>();

  return resolveRootItem(parent.dynamicCast<AbstractNamespaceItem>());
}

void KeysTreeRenderer::renderKeys(QSharedPointer<Operations> operations,
                                  RedisClient::Connection::RawKeysList keys,
                                  QSharedPointer<AbstractNamespaceItem> parent,
                                  RenderingSettigns settings,
                                  const QSet<QByteArray> &expandedNamespaces) {
  // init
  QElapsedTimer timer;
  timer.start();

  if (settings.sortKeys)
    std::sort(keys.begin(), keys.end());

  qDebug() << "Keys sorted in: " << timer.elapsed() << " ms";

  // render
  timer.restart();
  int unprocessedPartStart = 0;
  if (parent->getFullPath().size() > 0 || parent->type() == "namespace") {
    unprocessedPartStart =
        parent->getFullPath().size() + settings.nsSeparator.length();
  }

  QByteArray rawKey;
  QByteArray nextKey;

  auto rootItem = resolveRootItem(parent);

  QHash<QByteArray, QWeakPointer<KeyItem>> preRenderedKeys;

  if (rootItem) {
      preRenderedKeys = rootItem->getKeysIndex();
  }
  qDebug() << "Pre-rendered keys: " << preRenderedKeys.size();

  auto preRenderedKeysList = preRenderedKeys.keys();
  QSet<QByteArray> preRenderedKeysToBeRemoved;

  if (settings.checkPreRenderedItems) {
    preRenderedKeysToBeRemoved = QSet<QByteArray>(preRenderedKeysList.begin(),
                                                  preRenderedKeysList.end());
  }

  qDebug() << "Live update: " << settings.checkPreRenderedItems;
  qDebug() << "Fetching:" << (preRenderedKeys.size() > 0);

  while (!keys.isEmpty()) {
    rawKey = keys.takeFirst();

    if (preRenderedKeysList.contains(rawKey)) {
        if (preRenderedKeysToBeRemoved.contains(rawKey)) {
            preRenderedKeysToBeRemoved.remove(rawKey);
        }
        continue;
    }

    if (keys.size() > 0) {
        nextKey = keys[0];
    } else {
        nextKey = QByteArray();
    }

    try {
      renderLazily(rootItem, parent, rawKey.mid(unprocessedPartStart), rawKey, operations,
                   settings, expandedNamespaces, 0, nextKey);
    } catch (std::bad_alloc &) {
      parent->showLoadingError("Not enough memory to render all keys");
      break;
    }
  }
  qDebug() << "Tree builded in: " << timer.elapsed() << " ms";

  if (preRenderedKeysToBeRemoved.size() > 0) {
    QList<QWeakPointer<KeyItem>> obsoleteKeys;

    for (const auto &keyFullPath : qAsConst(preRenderedKeysToBeRemoved)) {
      obsoleteKeys.append(preRenderedKeys[keyFullPath]);
    }

    parent->removeObsoleteKeys(obsoleteKeys);
  }
}

void KeysTreeRenderer::renderLazily(QSharedPointer<AbstractNamespaceItem> root,
    QSharedPointer<AbstractNamespaceItem> parent,
    const QByteArray &notProcessedKeyPart, const QByteArray &fullKey,
    QSharedPointer<Operations> m_operations, const RenderingSettigns &settings,
    const QSet<QByteArray> &expandedNamespaces, unsigned long level,
    const QByteArray &nextKey) {
  Q_ASSERT(parent);

  if (level > 0 && parent->isExpanded() == false) {
    parent->appendRawKey(fullKey);
    return;
  }

  QWeakPointer<TreeItem> currentParent =
      parent.staticCast<TreeItem>().toWeakRef();

  int indexOfNaspaceSeparator =
      (settings.nsSeparator.isEmpty())
          ? -1
          : notProcessedKeyPart.indexOf(settings.nsSeparator);  

  if (indexOfNaspaceSeparator == -1) {
    if (parent->getAllChilds().size() >= settings.renderLimit) {
        parent->appendRawKey(fullKey);
    } else {
        QSharedPointer<KeyItem> newKey(
            new KeyItem(fullKey, currentParent, parent->model()));

        if (settings.appendNewItems) {
            parent->append(newKey);
        } else {
            parent->insertChild(newKey);
        }

        if (root && root->type() == "database") {
            root->appendKeyToIndex(newKey);
        }
    }
    return;
  }

  QByteArray firstNamespaceName =
      notProcessedKeyPart.mid(0, indexOfNaspaceSeparator);

  QSharedPointer<AbstractNamespaceItem> namespaceItem =
      parent->findChildNamespace(firstNamespaceName);

  if (namespaceItem.isNull()) {
    long nsPos =
        fullKey.size() - notProcessedKeyPart.size() + firstNamespaceName.size();
    QByteArray namespaceFullPath = fullKey.mid(0, nsPos);

    // Single namespaced key
    if (nextKey.isEmpty() || nextKey.indexOf(namespaceFullPath) == -1) {
        QSharedPointer<KeyItem> newKey(
            new KeyItem(fullKey, currentParent, parent->model()));
        parent->append(newKey);
        return;
    }

    namespaceItem = QSharedPointer<NamespaceItem>(
        new NamespaceItem(namespaceFullPath, m_operations, currentParent,
                          parent->model(), settings.dbIndex, settings.filter));

    if (expandedNamespaces.contains(namespaceFullPath)) {
      namespaceItem->setExpanded(true);
    }

    parent->appendNamespace(namespaceItem);
  }

  renderLazily(root, namespaceItem,
               notProcessedKeyPart.mid(indexOfNaspaceSeparator +
                                       settings.nsSeparator.length()),
               fullKey, m_operations, settings, expandedNamespaces,
               level + 1, nextKey);
}
