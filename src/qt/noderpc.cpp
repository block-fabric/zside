// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/noderpc.h>

#include <interfaces/node.h>
#include <qt/clientmodel.h>

#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <QTableWidgetItem>
#include <QThread>
#include <QUrl>

namespace NodeRpc {
namespace {
std::optional<UniValue> Execute(interfaces::Node& node, const std::string& method, const UniValue& params, QString& error, const std::optional<QString>& wallet)
{
    std::string uri{"/"};
    if (wallet) uri = "/wallet/" + QString::fromUtf8(QUrl::toPercentEncoding(*wallet)).toStdString();
    try {
        return node.executeRpc(method, params, uri);
    } catch (const UniValue& rpc_error) {
        error = rpc_error.isObject() && rpc_error.exists("message") ? Text(rpc_error["message"]) : QString::fromStdString(rpc_error.write());
    } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
    }
    return std::nullopt;
}

/** The object, on a thread of its own, that runs the calls of CallAsync. Made on first use, on the GUI thread. */
QObject* Worker()
{
    static QPointer<QObject> worker;
    if (!worker) {
        auto* thread{new QThread(qApp)};
        thread->setObjectName(QStringLiteral("noderpc"));
        worker = new QObject;
        worker->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, worker.data(), &QObject::deleteLater);
        // A call under way is waited for: it holds on to the node.
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, thread, [thread] {
            thread->quit();
            thread->wait();
        });
        thread->start();
    }
    return worker;
}
} // namespace

std::optional<UniValue> Call(ClientModel* client_model, const std::string& method, const UniValue& params, QString& error, const std::optional<QString>& wallet)
{
    if (!client_model) {
        error = QObject::tr("The node is not available.");
        return std::nullopt;
    }
    return Execute(client_model->node(), method, params, error, wallet);
}

void CallAsync(QObject* receiver, ClientModel* client_model, const std::string& method, const UniValue& params, Done done, const std::optional<QString>& wallet)
{
    if (!client_model) {
        done(std::nullopt, QObject::tr("The node is not available."));
        return;
    }
    interfaces::Node* node{&client_model->node()};
    QPointer<QObject> guard{receiver};
    QMetaObject::invokeMethod(Worker(), [node, method, params, wallet, guard, done = std::move(done)] {
        QString error;
        std::optional<UniValue> result{Execute(*node, method, params, error, wallet)};
        // Back on the GUI thread, for a receiver that is still there.
        QMetaObject::invokeMethod(qApp, [guard, done, result = std::move(result), error] {
            if (guard) done(result, error);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

UniValue Args(std::initializer_list<UniValue> values)
{
    UniValue params(UniValue::VARR);
    for (const auto& value : values) params.push_back(value);
    return params;
}

QString Text(const UniValue& value)
{
    return value.isStr() ? QString::fromStdString(value.get_str()) : QString::fromStdString(value.getValStr());
}

QTableWidgetItem* Item(const QString& text)
{
    auto* item{new QTableWidgetItem(text)};
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString HashRate(double hashes_per_second)
{
    static const char* const UNITS[]{"H/s", "kH/s", "MH/s", "GH/s", "TH/s", "PH/s", "EH/s"};
    size_t unit{0};
    while (hashes_per_second >= 1000 && unit + 1 < std::size(UNITS)) {
        hashes_per_second /= 1000;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(hashes_per_second, 0, 'f', unit == 0 ? 0 : 2).arg(UNITS[unit]);
}

} // namespace NodeRpc
