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

#include <atomic>

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

//! Set by Stop (on the GUI thread): calls queued are skipped, and no new one is taken.
std::atomic<bool> g_stopped{false};
//! The thread of the worker, once made (GUI thread only).
QPointer<QThread> g_thread;

/** The object, on a thread of its own, that runs the calls of CallAsync. Made on first use, on the GUI thread. */
QObject* Worker()
{
    static QPointer<QObject> worker;
    // Made again after Restart: the thread of the one before was stopped.
    if (!worker || !g_thread || g_thread->isFinished()) {
        auto* thread{new QThread(qApp)};
        thread->setObjectName(QStringLiteral("noderpc"));
        worker = new QObject;
        worker->moveToThread(thread);
        QObject::connect(thread, &QThread::finished, worker.data(), &QObject::deleteLater);
        // In case Stop was not called (it is, before the node shuts down).
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, thread, &Stop);
        g_thread = thread;
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
    RunAsync(receiver, [node, method, params, wallet, done = std::move(done)]() -> std::function<void()> {
        QString error;
        std::optional<UniValue> result{Execute(*node, method, params, error, wallet)};
        return [done, result = std::move(result), error] { done(result, error); };
    });
}

void RunAsync(QObject* receiver, std::function<std::function<void()>()> work)
{
    // Dropped once the node shuts down: the receiver never hears of it, which no longer matters.
    if (g_stopped) return;
    QPointer<QObject> guard{receiver};
    QMetaObject::invokeMethod(Worker(), [guard, work = std::move(work)] {
        // Queued before Stop: skipped, the node is shutting down.
        if (g_stopped) return;
        std::function<void()> then{work()};
        // Back on the GUI thread, for a receiver that is still there.
        QMetaObject::invokeMethod(qApp, [guard, then = std::move(then)] {
            if (guard && then) then();
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void Stop()
{
    g_stopped = true;
    if (!g_thread) return;
    // A call under way is waited for: it holds on to the node.
    g_thread->quit();
    g_thread->wait();
}

void Restart()
{
    Stop();
    g_stopped = false;
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
