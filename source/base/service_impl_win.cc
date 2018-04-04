//
// PROJECT:         Aspia
// FILE:            base/service_impl_win.cc
// LICENSE:         GNU Lesser General Public License 2.1
// PROGRAMMERS:     Dmitry Chapyshev (dmitry@aspia.ru)
//

#include "base/service_impl.h"

#if !defined(Q_OS_WIN)
#error This file for MS Windows only
#endif // defined(Q_OS_WIN)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <QCoreApplication>
#include <QDebug>
#include <QMutex>
#include <QSemaphore>
#include <QThread>
#include <QWaitCondition>

#include "base/system_error_code.h"

namespace aspia {

class ServiceHandler : public QThread
{
public:
    ServiceHandler();

    void setStatus(DWORD status);

    static ServiceHandler* instance;

    QSemaphore create_app_start_semaphore;
    QSemaphore create_app_end_semaphore;

    QWaitCondition event_condition;
    QMutex event_lock;
    bool event_processed = false;

protected:
    // QThread implementation.
    void run() override;

private:
    static void WINAPI serviceMain(DWORD argc, LPWSTR* argv);
    static DWORD WINAPI serviceControlHandler(
        DWORD control_code, DWORD event_type, LPVOID event_data, LPVOID context);

    SERVICE_STATUS_HANDLE status_handle_ = nullptr;
    SERVICE_STATUS status_;

    Q_DISABLE_COPY(ServiceHandler)
};

class ServiceEventHandler : public QObject
{
public:
    ServiceEventHandler();

    static ServiceEventHandler* instance;

    enum ServiceEventType
    {
        Start         = QEvent::User + 1,
        Stop          = QEvent::User + 2,
        SessionChange = QEvent::User + 3
    };

    static void postStartEvent();
    static void postStopEvent();
    static void postSessionChangeEvent(quint32 event, quint32 session_id);

    class SessionChangeEvent : public QEvent
    {
    public:
        SessionChangeEvent(quint32 event, quint32 session_id)
            : QEvent(QEvent::Type(SessionChange)),
            event_(event),
            session_id_(session_id)
        {
            // Nothing
        }

        quint32 event() const { return event_; }
        quint32 sessionId() const { return session_id_; }

    private:
        quint32 event_;
        quint32 session_id_;

        Q_DISABLE_COPY(SessionChangeEvent)
    };

protected:
    // QObject implementation.
    void customEvent(QEvent* event) override;

private:
    Q_DISABLE_COPY(ServiceEventHandler)
};

//================================================================================================
// ServiceHandler implementation.
//================================================================================================

ServiceHandler* ServiceHandler::instance = nullptr;

ServiceHandler::ServiceHandler()
{
    Q_ASSERT(!instance);
    instance = this;

    memset(&status_, 0, sizeof(status_));
}

void ServiceHandler::setStatus(DWORD status)
{
    status_.dwServiceType             = SERVICE_WIN32;
    status_.dwControlsAccepted        = 0;
    status_.dwCurrentState            = status;
    status_.dwWin32ExitCode           = NO_ERROR;
    status_.dwServiceSpecificExitCode = NO_ERROR;
    status_.dwWaitHint                = 0;

    if (status == SERVICE_RUNNING)
    {
        status_.dwControlsAccepted =
            SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    }

    if (status != SERVICE_RUNNING && status != SERVICE_STOPPED)
        ++status_.dwCheckPoint;
    else
        status_.dwCheckPoint = 0;

    if (!SetServiceStatus(status_handle_, &status_))
    {
        qWarning() << "SetServiceStatus failed: " << lastSystemErrorString();
        return;
    }
}

void ServiceHandler::run()
{
    SERVICE_TABLE_ENTRYW service_table[1];
    memset(&service_table, 0, sizeof(service_table));

    service_table[0].lpServiceName = const_cast<wchar_t*>(
        reinterpret_cast<const wchar_t*>(ServiceImpl::instance()->serviceName().utf16()));
    service_table[0].lpServiceProc = ServiceHandler::serviceMain;

    if (!StartServiceCtrlDispatcherW(service_table))
    {
        qWarning() << "StartServiceCtrlDispatcherW failed: " << lastSystemErrorString();
        return;
    }
}

// static
void WINAPI ServiceHandler::serviceMain(DWORD /* argc */, LPWSTR* /* argv */)
{
    Q_ASSERT(instance);

    // Start creating the QCoreApplication instance.
    instance->create_app_start_semaphore.release();

    // Waiting for the completion of the creation.
    instance->create_app_end_semaphore.acquire();

    instance->status_handle_ = RegisterServiceCtrlHandlerExW(
        reinterpret_cast<const wchar_t*>(ServiceImpl::instance()->serviceName().utf16()),
        serviceControlHandler,
        nullptr);

    if (!instance->status_handle_)
    {
        qWarning() << "RegisterServiceCtrlHandlerExW failed: " << lastSystemErrorString();
        return;
    }

    instance->setStatus(SERVICE_START_PENDING);
    instance->event_lock.lock();
    instance->event_processed = false;

    ServiceEventHandler::postStartEvent();

    // Wait for the event to be processed by the application.
    while (!instance->event_processed)
        instance->event_condition.wait(&instance->event_lock);

    instance->event_lock.unlock();
    instance->setStatus(SERVICE_RUNNING);
}

// static
DWORD WINAPI ServiceHandler::serviceControlHandler(
    DWORD control_code, DWORD event_type, LPVOID event_data, LPVOID /* context */)
{
    switch (control_code)
    {
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;

        case SERVICE_CONTROL_SESSIONCHANGE:
        {
            Q_ASSERT(instance);

            instance->event_lock.lock();
            instance->event_processed = false;

            // Post event to application.
            ServiceEventHandler::postSessionChangeEvent(
                event_type, reinterpret_cast<WTSSESSION_NOTIFICATION*>(event_data)->dwSessionId);

            // Wait for the event to be processed by the application.
            while (!instance->event_processed)
                instance->event_condition.wait(&instance->event_lock);

            instance->event_lock.unlock();
        }
        return NO_ERROR;

        case SERVICE_CONTROL_SHUTDOWN:
        case SERVICE_CONTROL_STOP:
        {
            Q_ASSERT(instance);

            if (control_code == SERVICE_CONTROL_STOP)
                instance->setStatus(SERVICE_STOP_PENDING);

            instance->event_lock.lock();
            instance->event_processed = false;

            // Post event to application.
            ServiceEventHandler::postStopEvent();

            // Wait for the event to be processed by the application.
            while (!instance->event_processed)
                instance->event_condition.wait(&instance->event_lock);

            instance->event_lock.unlock();
        }
        return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

//================================================================================================
// ServiceEventHandler implementation.
//================================================================================================

ServiceEventHandler* ServiceEventHandler::instance = nullptr;

ServiceEventHandler::ServiceEventHandler()
{
    Q_ASSERT(!instance);
    instance = this;
}

// static
void ServiceEventHandler::postStartEvent()
{
    QCoreApplication::postEvent(instance, new QEvent(QEvent::Type(Start)));
}

// static
void ServiceEventHandler::postStopEvent()
{
    QCoreApplication::postEvent(instance, new QEvent(QEvent::Type(Stop)));
}

// static
void ServiceEventHandler::postSessionChangeEvent(quint32 event, quint32 session_id)
{
    QCoreApplication::postEvent(instance, new SessionChangeEvent(event, session_id));
}

void ServiceEventHandler::customEvent(QEvent* event)
{
    switch (event->type())
    {
        case ServiceEventHandler::Start:
            ServiceImpl::instance()->start();
            break;

        case ServiceEventHandler::Stop:
            ServiceImpl::instance()->stop();
            QCoreApplication::instance()->quit();
            break;

        case ServiceEventHandler::SessionChange:
        {
            SessionChangeEvent* session_change_event =
                reinterpret_cast<SessionChangeEvent*>(event);

            ServiceImpl::instance()->sessionChange(
                session_change_event->event(), session_change_event->sessionId());
        }
        break;

        default:
            return;
    }

    // Set the event flag is processed.
    ServiceHandler::instance->event_lock.lock();
    ServiceHandler::instance->event_processed = true;
    ServiceHandler::instance->event_lock.unlock();

    // Notify waiting thread for the end of processing.
    ServiceHandler::instance->event_condition.notify_one();
}

//================================================================================================
// ServiceImpl implementation.
//================================================================================================

ServiceImpl* ServiceImpl::instance_ = nullptr;

ServiceImpl::ServiceImpl(const QString& name)
    : name_(name)
{
    // Nothing
}

int ServiceImpl::exec(int argc, char* argv[])
{
    QScopedPointer<ServiceHandler> handler(new ServiceHandler());

    // Starts handler thread.
    handler->start();

    // Waiting for the launch ServiceHandler::serviceMain.
    if (!handler->create_app_start_semaphore.tryAcquire(1, 20000))
    {
        qWarning("Function serviceMain was not called at the specified time interval");
        return 1;
    }

    // Creates QCoreApplication.
    createApplication(argc, argv);
    Q_ASSERT(QCoreApplication::instance());

    QScopedPointer<ServiceEventHandler> event_handler(new ServiceEventHandler());

    // Now we can complete the registration of the service.
    handler->create_app_end_semaphore.release();

    // Calls QCoreApplication::exec().
    int ret = executeApplication();

    // Set the state of the service and wait for the thread to complete.
    handler->setStatus(SERVICE_STOPPED);
    handler->wait();

    event_handler.reset();

    return ret;
}

} // namespace aspia
