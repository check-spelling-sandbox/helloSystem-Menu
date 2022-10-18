/* This file is part of the dbusmenu-qt library
   Copyright 2009 Canonical
   Author: Aurelien Gateau <aurelien.gateau@canonical.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License (LGPL) as published by the Free Software Foundation;
   either version 2 of the License, or (at your option) any later
   version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/
#include "dbusmenuimporter.h"

//#include "debug.h"

// Qt
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#include <QFont>
#include <QMenu>
#include <QPointer>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QWidgetAction>
#include <QSet>
#include <QDebug>

// Local
#include "dbusmenutypes_p.h"
#include "dbusmenushortcut_p.h"
#include "utils_p.h"

// Generated
#include "dbusmenu_interface.h"

//#define BENCHMARK
#ifdef BENCHMARK
    static QTime sChrono;
#endif

#define DMRETURN_IF_FAIL(cond) if (!(cond)) { \
        qWarning() << "Condition failed: " #cond; \
        return; \
    }

static const char *DBUSMENU_PROPERTY_ID = "_dbusmenu_id";
static const char *DBUSMENU_PROPERTY_ICON_NAME = "_dbusmenu_icon_name";
static const char *DBUSMENU_PROPERTY_ICON_DATA_HASH = "_dbusmenu_icon_data_hash";

static QAction *createKdeTitle(QAction *action, QWidget *parent)
{
    QToolButton *titleWidget = new QToolButton(nullptr);
    QFont font = titleWidget->font();
    font.setBold(true);
    titleWidget->setFont(font);
    titleWidget->setIcon(action->icon());
    titleWidget->setText(action->text());
    titleWidget->setDown(true);
    titleWidget->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    QWidgetAction *titleAction = new QWidgetAction(parent);
    titleAction->setDefaultWidget(titleWidget);
    return titleAction;
}

class DBusMenuImporterPrivate
{
public:
    DBusMenuImporter *q;

    ComCanonicalDbusmenuInterface *m_interface;
    QMenu *m_menu;
    using ActionForId = QMap<int, QAction * >;
    ActionForId m_actionForId;
    QTimer *m_pendingLayoutUpdateTimer;

    QSet<int> m_idsRefreshedByAboutToShow;
    QSet<int> m_pendingLayoutUpdates;

    QDBusPendingCallWatcher *refresh(int id) {
        auto call = m_interface->GetLayout(id, 1, QStringList());
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, q);
        watcher->setProperty(DBUSMENU_PROPERTY_ID, id);
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                         q, &DBusMenuImporter::slotGetLayoutFinished);

        return watcher;
    }

    QMenu *createMenu(QWidget *parent) {
        QMenu *menu = q->createMenu(parent);
        return menu;
    }

    /**
     * Init all the immutable action properties here
     * TODO: Document immutable properties?
     *
     * Note: we remove properties we handle from the map (using QMap::take()
     * instead of QMap::value()) to avoid warnings about these properties in
     * updateAction()
     */
    QAction *createAction(int id, const QVariantMap &_map, QWidget *parent) {
        QVariantMap map = _map;
        QAction *action = new QAction(parent);
        action->setProperty(DBUSMENU_PROPERTY_ID, id);

        QString type = map.take(QStringLiteral("type")).toString();

        if (type == QLatin1String("separator")) {
            action->setSeparator(true);
        }

        if (map.take(QStringLiteral("children-display")).toString() == QLatin1String("submenu")) {
            QMenu *menu = createMenu(parent);
            action->setMenu(menu);
        }

        QString toggleType = map.take(QStringLiteral("toggle-type")).toString();

        if (!toggleType.isEmpty()) {
            action->setCheckable(true);

            if (toggleType == QLatin1String("radio")) {
                QActionGroup *group = new QActionGroup(action);
                group->addAction(action);
            }
        }

        bool isKdeTitle = map.take(QStringLiteral("x-kde-title")).toBool();
        updateAction(action, map, map.keys());

        if (isKdeTitle) {
            action = createKdeTitle(action, parent);
        }

        return action;
    }

    /**
     * Update mutable properties of an action. A property may be listed in
     * requestedProperties but not in map, this means we should use the default value
     * for this property.
     *
     * @param action the action to update
     * @param map holds the property values
     * @param requestedProperties which properties has been requested
     */
    void updateAction(QAction *action, const QVariantMap &map, const QStringList &requestedProperties) {
        Q_FOREACH (const QString &key, requestedProperties) {
            updateActionProperty(action, key, map.value(key));
        }
    }

    void updateActionProperty(QAction *action, const QString &key, const QVariant &value) {
        if (key == QLatin1String("label")) {
            updateActionLabel(action, value);
        } else if (key == QLatin1String("enabled")) {
            updateActionEnabled(action, value);
        } else if (key == QLatin1String("toggle-state")) {
            updateActionChecked(action, value);
        } else if (key == QLatin1String("icon-name")) {
            // updateActionIconByName(action, value); // probono: No icons in menus
        } else if (key == QLatin1String("icon-data")) {
            // updateActionIconByData(action, value); // probono: No icons in menus
        } else if (key == QLatin1String("visible")) {
            updateActionVisible(action, value);
        } else if (key == QLatin1String("shortcut")) {
            updateActionShortcut(action, value);
        } else {
//            qDebug() << "Unhandled property update" << key;
        }
    }

    void updateActionLabel(QAction *action, const QVariant &value) {
        QString text = swapMnemonicChar(value.toString(), '_', '&');
        // probono: https://github.com/helloSystem/Menu/issues/75
        if(text.replace("&", "") == "Datei"){
            text = "Ablage";
        }
        action->setText(text);
    }

    void updateActionEnabled(QAction *action, const QVariant &value) {
        action->setEnabled(value.isValid() ? value.toBool() : true);
    }

    void updateActionChecked(QAction *action, const QVariant &value) {
        if (action->isCheckable() && value.isValid()) {
            action->setChecked(value.toInt() == 1);
        }
    }

    void updateActionIconByName(QAction *action, const QVariant &value) {
        const QString iconName = value.toString();
        const QString previous = action->property(DBUSMENU_PROPERTY_ICON_NAME).toString();

        if (previous == iconName) {
            return;
        }

        action->setProperty(DBUSMENU_PROPERTY_ICON_NAME, iconName);

        if (iconName.isEmpty()) {
            action->setIcon(QIcon());
            return;
        }

        action->setIcon(q->iconForName(iconName));
    }

    void updateActionIconByData(QAction *action, const QVariant &value) {
        const QByteArray data = value.toByteArray();
        uint dataHash = qHash(data);
        uint previousDataHash = action->property(DBUSMENU_PROPERTY_ICON_DATA_HASH).toUInt();

        if (previousDataHash == dataHash) {
            return;
        }

        action->setProperty(DBUSMENU_PROPERTY_ICON_DATA_HASH, dataHash);
        QPixmap pix;

        if (!pix.loadFromData(data)) {
            qDebug() << "Failed to decode icon-data property for action" << action->text();
            action->setIcon(QIcon());
            return;
        }

        action->setIcon(QIcon(pix));
    }

    void updateActionVisible(QAction *action, const QVariant &value) {
        action->setVisible(value.isValid() ? value.toBool() : true);
    }

    void updateActionShortcut(QAction *action, const QVariant &value) {
        QDBusArgument arg = value.value<QDBusArgument>();
        DBusMenuShortcut dmShortcut;
        arg >> dmShortcut;
        QKeySequence keySequence = dmShortcut.toKeySequence();
        action->setShortcut(keySequence);
    }

    QMenu *menuForId(int id) const {
        if (id == 0) {
            return q->menu();
        }

        QAction *action = m_actionForId.value(id);

        if (!action) {
            return nullptr;
        }

        return action->menu();
    }

    void slotItemsPropertiesUpdated(const DBusMenuItemList &updatedList, const DBusMenuItemKeysList &removedList);

    void sendEvent(int id, const QString &eventId) {
        m_interface->Event(id, eventId, QDBusVariant(QString()), 0u);
    }
};

DBusMenuImporter::DBusMenuImporter(const QString &service, const QString &path, QObject *parent)
    : QObject(parent)
    , d(new DBusMenuImporterPrivate)
{
    DBusMenuTypes_register();

    d->q = this;
    d->m_interface = new ComCanonicalDbusmenuInterface(service, path, QDBusConnection::sessionBus(), this);
    d->m_menu = nullptr;

    d->m_pendingLayoutUpdateTimer = new QTimer(this);
    d->m_pendingLayoutUpdateTimer->setSingleShot(true);
    connect(d->m_pendingLayoutUpdateTimer, &QTimer::timeout, this, &DBusMenuImporter::processPendingLayoutUpdates);

    connect(d->m_interface, &ComCanonicalDbusmenuInterface::LayoutUpdated, this, &DBusMenuImporter::slotLayoutUpdated);
    connect(d->m_interface, &ComCanonicalDbusmenuInterface::ItemActivationRequested, this, &DBusMenuImporter::slotItemActivationRequested);
    connect(d->m_interface, &ComCanonicalDbusmenuInterface::ItemsPropertiesUpdated, this, [this](const DBusMenuItemList & updatedList, const DBusMenuItemKeysList & removedList) {
        d->slotItemsPropertiesUpdated(updatedList, removedList);
    });

    d->refresh(0);
}

DBusMenuImporter::~DBusMenuImporter()
{
    // Do not use "delete d->m_menu": even if we are being deleted we should
    // leave enough time for the menu to finish what it was doing, for example
    // if it was being displayed.
    d->m_menu->deleteLater();
    delete d;
}

void DBusMenuImporter::slotLayoutUpdated(uint revision, int parentId)
{
    Q_UNUSED(revision)

    if (d->m_idsRefreshedByAboutToShow.remove(parentId)) {
        return;
    }

    d->m_pendingLayoutUpdates << parentId;

    if (!d->m_pendingLayoutUpdateTimer->isActive()) {
        d->m_pendingLayoutUpdateTimer->start();
    }
}

void DBusMenuImporter::processPendingLayoutUpdates()
{
    QSet<int> ids = d->m_pendingLayoutUpdates;
    d->m_pendingLayoutUpdates.clear();

    Q_FOREACH (int id, ids) {
        d->refresh(id);
    }
}

QMenu *DBusMenuImporter::menu() const
{
    if (!d->m_menu) {
        d->m_menu = d->createMenu(nullptr);
    }

    return d->m_menu;
}

void DBusMenuImporterPrivate::slotItemsPropertiesUpdated(const DBusMenuItemList &updatedList, const DBusMenuItemKeysList &removedList)
{
    Q_FOREACH (const DBusMenuItem &item, updatedList) {
        QAction *action = m_actionForId.value(item.id);

        if (!action) {
            // We don't know this action. It probably is in a menu we haven't fetched yet.
            continue;
        }

        QVariantMap::ConstIterator
        it = item.properties.constBegin(),
        end = item.properties.constEnd();

        for (; it != end; ++it) {
            updateActionProperty(action, it.key(), it.value());
        }
    }

    Q_FOREACH (const DBusMenuItemKeys &item, removedList) {
        QAction *action = m_actionForId.value(item.id);

        if (!action) {
            // We don't know this action. It probably is in a menu we haven't fetched yet.
            continue;
        }

        Q_FOREACH (const QString &key, item.properties) {
            updateActionProperty(action, key, QVariant());
        }
    }
}

QAction *DBusMenuImporter::actionForId(int id) const
{
    return d->m_actionForId.value(id);
}

void DBusMenuImporter::slotItemActivationRequested(int id, uint /*timestamp*/)
{
    QAction *action = d->m_actionForId.value(id);
    DMRETURN_IF_FAIL(action);
    actionActivationRequested(action);
}

void DBusMenuImporter::slotGetLayoutFinished(QDBusPendingCallWatcher *watcher)
{
    int parentId = watcher->property(DBUSMENU_PROPERTY_ID).toInt();
    watcher->deleteLater();

    QMenu *menu = d->menuForId(parentId);

    QDBusPendingReply<uint, DBusMenuLayoutItem> reply = *watcher;

    if (!reply.isValid()) {
        qDebug() << reply.error().message();

        if (menu) {
            emit menuUpdated(menu);
        }

        return;
    }

#ifdef BENCHMARK
    DMDEBUG << "- items received:" << sChrono.elapsed() << "ms";
#endif
    DBusMenuLayoutItem rootItem = reply.argumentAt<1>();

    if (!menu) {
//        qDebug() << "No menu for id" << parentId;
        return;
    }

    //remove outdated actions
    QSet<int> newDBusMenuItemIds;
    newDBusMenuItemIds.reserve(rootItem.children.count());

    for (const DBusMenuLayoutItem &item : rootItem.children) {
        newDBusMenuItemIds << item.id;
    }

    for (QAction *action : menu->actions()) {
        int id = action->property(DBUSMENU_PROPERTY_ID).toInt();

        if (! newDBusMenuItemIds.contains(id)) {
            // Not calling removeAction() as QMenu will immediately close when it becomes empty,
            // which can happen when an application completely reloads this menu.
            // When the action is deleted deferred, it is removed from the menu.
            action->deleteLater();
            d->m_actionForId.remove(id);
        }
    }

    //insert or update new actions into our menu
    for (const DBusMenuLayoutItem &dbusMenuItem : rootItem.children) {
        DBusMenuImporterPrivate::ActionForId::Iterator it = d->m_actionForId.find(dbusMenuItem.id);
        QAction *action = nullptr;

        if (it == d->m_actionForId.end()) {
            int id = dbusMenuItem.id;
            action = d->createAction(id, dbusMenuItem.properties, menu);
            d->m_actionForId.insert(id, action);

            connect(action, &QObject::destroyed, this, [this, id]() {
                d->m_actionForId.remove(id);
            });

            connect(action, &QAction::triggered, this, [action, id, this]() {
                sendClickedEvent(id);
            });

            /*
             * This leads to DBus activity, high CPU usage, and visual stutter
             * when the user moves the mouse across the menu bar
             * while menus are open. Why was this there in the first place?
             * https://github.com/helloSystem/Menu/issues/93#issuecomment-1282718239
            if (QMenu *menuAction = action->menu()) {
                connect(menuAction, &QMenu::aboutToShow, this, &DBusMenuImporter::slotMenuAboutToShow, Qt::UniqueConnection);
            }

            connect(menu, &QMenu::aboutToHide, this, &DBusMenuImporter::slotMenuAboutToHide, Qt::UniqueConnection);
            */

            menu->addAction(action);
        } else {
            action = *it;
            QStringList filteredKeys = dbusMenuItem.properties.keys();
            filteredKeys.removeOne("type");
            filteredKeys.removeOne("toggle-type");
            filteredKeys.removeOne("children-display");
            d->updateAction(*it, dbusMenuItem.properties, filteredKeys);
            // Move the action to the tail so we can keep the order same as the dbus request.
            menu->removeAction(action);
            menu->addAction(action);
        }
    }

    emit menuUpdated(menu);
}

void DBusMenuImporter::sendClickedEvent(int id)
{
    d->sendEvent(id, QStringLiteral("clicked"));
}

void DBusMenuImporter::updateMenu()
{
    updateMenu(DBusMenuImporter::menu());
}

void DBusMenuImporter::updateMenu(QMenu *menu)
{
    Q_ASSERT(menu);

    QAction *action = menu->menuAction();
    Q_ASSERT(action);

    int id = action->property(DBUSMENU_PROPERTY_ID).toInt();

    auto call = d->m_interface->AboutToShow(id);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);
    watcher->setProperty(DBUSMENU_PROPERTY_ID, id);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            &DBusMenuImporter::slotAboutToShowDBusCallFinished);

    // Firefox deliberately ignores "aboutToShow" whereas Qt ignores" opened", so we'll just send both all the time...
    d->sendEvent(id, QStringLiteral("opened"));
}


void DBusMenuImporter::slotAboutToShowDBusCallFinished(QDBusPendingCallWatcher *watcher)
{
    int id = watcher->property(DBUSMENU_PROPERTY_ID).toInt();
    watcher->deleteLater();

    QMenu *menu = d->menuForId(id);

    if (!menu) {
        return;
    }

    QDBusPendingReply<bool> reply = *watcher;

    if (reply.isError()) {
        qDebug() << "Call to AboutToShow() failed:" << reply.error().message();
        menuUpdated(menu);
	return;
    }

    //Note, this isn't used by Qt's QPT - but we get a LayoutChanged emitted before
    //this returns, which equates to the same thing
    bool needRefresh = reply.argumentAt<0>();

    if (needRefresh || menu->actions().isEmpty()) {
        d->m_idsRefreshedByAboutToShow << id;
        d->refresh(id);
    } else if (menu) {
	emit menuUpdated(menu);
    }
}

void DBusMenuImporter::slotMenuAboutToHide()
{
    QMenu *menu = qobject_cast<QMenu *>(sender());
    Q_ASSERT(menu);

    QAction *action = menu->menuAction();
    Q_ASSERT(action);

    int id = action->property(DBUSMENU_PROPERTY_ID).toInt();
    d->sendEvent(id, QStringLiteral("closed"));
}

void DBusMenuImporter::slotMenuAboutToShow()
{
    QMenu *menu = qobject_cast<QMenu *>(sender());
    Q_ASSERT(menu);

    //! update colors to sub-menus based on the parent menu colors
    if (menu && menu->parent()) {
        QMenu *parent_menu = qobject_cast<QMenu *>(menu->parent()->parent());
        if (parent_menu) {
            QMenu *sub_menu = qobject_cast<QMenu *>(menu->parent());
            if (sub_menu) {
                menu->setPalette(sub_menu->palette());
            }
        }
    }

    updateMenu(menu);
}

QMenu *DBusMenuImporter::createMenu(QWidget *parent)
{
    return new QMenu(parent);
}

QIcon DBusMenuImporter::iconForName(const QString &/*name*/)
{
    return QIcon();
}

#include "moc_dbusmenuimporter.cpp"
