#include "loadmoreitem.h"

ConnectionsTree::LoadMoreItem::LoadMoreItem(QWeakPointer<TreeItem> parent, Model &model)
    : ConnectionsTree::TreeItem::TreeItem(model),
      m_parent(parent)
{

}

QString ConnectionsTree::LoadMoreItem::getDisplayName() const
{
    return QString("Load more items");
}

QList<QSharedPointer<ConnectionsTree::TreeItem> > ConnectionsTree::LoadMoreItem::getAllChilds() const
{
    return {};
}

bool ConnectionsTree::LoadMoreItem::supportChildItems() const
{
    return false;
}

uint ConnectionsTree::LoadMoreItem::childCount(bool recursive) const
{
    return 0u;
}

QSharedPointer<ConnectionsTree::TreeItem> ConnectionsTree::LoadMoreItem::child(uint)
{
    return QSharedPointer<ConnectionsTree::TreeItem>();
}

QWeakPointer<ConnectionsTree::TreeItem> ConnectionsTree::LoadMoreItem::parent() const
{
    return m_parent;
}

bool ConnectionsTree::LoadMoreItem::isEnabled() const
{
    return true;
}

QHash<QString, std::function<void ()> > ConnectionsTree::LoadMoreItem::eventHandlers()
{
    QHash<QString, std::function<void()>> events;
    events["click"] = [this]() {
        auto parentPtr = m_parent.toStrongRef();
        if (!parentPtr) {
            return;
        }
        qDebug() << ">>>>>> Loading more items <<<<<<<<<<";
        parentPtr->fetchMore();
    };
    return events;
}
