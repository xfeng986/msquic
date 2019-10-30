/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    QUIC Listener API and Logic

--*/

#include "precomp.h"

#ifdef QUIC_LOGS_WPP
#include "listener.tmh"
#endif

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QUIC_API
MsQuicListenerOpen(
    _In_ _Pre_defensive_ HQUIC SessionHandle,
    _In_ _Pre_defensive_ QUIC_LISTENER_CALLBACK_HANDLER Handler,
    _In_opt_ void* Context,
    _Outptr_ _At_(*NewListener, __drv_allocatesMem(Mem)) _Pre_defensive_
        HQUIC *NewListener
    )
{
    QUIC_STATUS Status;
    PQUIC_SESSION Session;
    PQUIC_LISTENER Listener = NULL;

    EventWriteQuicApiEnter(
        QUIC_TRACE_API_LISTENER_OPEN,
        SessionHandle);

    if (SessionHandle == NULL ||
        SessionHandle->Type != QUIC_HANDLE_TYPE_SESSION ||
        NewListener == NULL ||
        Handler == NULL) {
        Status = QUIC_STATUS_INVALID_PARAMETER;
        goto Error;
    }

    Session = (PQUIC_SESSION)SessionHandle;

    Listener = QUIC_ALLOC_NONPAGED(sizeof(QUIC_LISTENER));
    if (Listener == NULL) {
        EventWriteQuicAllocFailure("listener", sizeof(QUIC_LISTENER));
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Error;
    }

    QuicZeroMemory(Listener, sizeof(QUIC_LISTENER));
    Listener->Type = QUIC_HANDLE_TYPE_LISTENER;
    Listener->Session = Session;
    Listener->ClientCallbackHandler = Handler;
    Listener->ClientContext = Context;
    QuicRundownInitializeDisabled(&Listener->Rundown);

#pragma prefast(suppress: __WARNING_6031, "Will always succeed.")
    QuicRundownAcquire(&Session->Rundown);

    EventWriteQuicListenerCreated(Listener, Listener->Session);
    *NewListener = (HQUIC)Listener;
    Status = QUIC_STATUS_SUCCESS;

Error:

    if (QUIC_FAILED(Status)) {

        if (Listener != NULL) {
            QUIC_FREE(Listener);
        }
    }

    EventWriteQuicApiExitStatus(Status);

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QUIC_API
MsQuicListenerClose(
    _In_ _Pre_defensive_ __drv_freesMem(Mem)
        HQUIC Handle
    )
{
    if (Handle == NULL) {
        return;
    }

    QUIC_TEL_ASSERT(Handle->Type == QUIC_HANDLE_TYPE_LISTENER);
    _Analysis_assume_(Handle->Type == QUIC_HANDLE_TYPE_LISTENER);
    if (Handle->Type != QUIC_HANDLE_TYPE_LISTENER) {
        return;
    }

    EventWriteQuicApiEnter(
        QUIC_TRACE_API_LISTENER_CLOSE,
        Handle);

#pragma prefast(suppress: __WARNING_25024, "Pointer cast already validated.")
    PQUIC_LISTENER Listener = (PQUIC_LISTENER)Handle;
    PQUIC_SESSION Session = Listener->Session;

    //
    // Make sure the listener has unregistered from the binding.
    //
    MsQuicListenerStop(Handle);

    QuicRundownUninitialize(&Listener->Rundown);
    QUIC_FREE(Listener);

    EventWriteQuicListenerDestroyed(Listener);
    QuicRundownRelease(&Session->Rundown);

    EventWriteQuicApiExit();
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QUIC_API
MsQuicListenerStart(
    _In_ _Pre_defensive_ HQUIC Handle,
    _In_opt_ const QUIC_ADDR * LocalAddress
    )
{
    QUIC_STATUS Status;
    PQUIC_LISTENER Listener;
    BOOLEAN PortUnspecified;
    QUIC_ADDR BindingLocalAddress = {0};

    EventWriteQuicApiEnter(
        QUIC_TRACE_API_LISTENER_START,
        Handle);

    if (Handle == NULL ||
        Handle->Type != QUIC_HANDLE_TYPE_LISTENER) {
        Status = QUIC_STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    if (LocalAddress && !QuicAddrIsValid(LocalAddress)) {
        Status = QUIC_STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    Status = QUIC_STATUS_SUCCESS;
#pragma prefast(suppress: __WARNING_25024, "Pointer cast already validated.")
    Listener = (PQUIC_LISTENER)Handle;

    if (Listener->Binding) {
        Status = QUIC_STATUS_INVALID_STATE;
        goto Exit;
    }

    if (LocalAddress != NULL) {
        QuicCopyMemory(&Listener->LocalAddress, LocalAddress, sizeof(QUIC_ADDR));
        Listener->WildCard = QuicAddrIsWildCard(LocalAddress);
        PortUnspecified = QuicAddrGetPort(LocalAddress) == 0;
    } else {
        QuicZeroMemory(&Listener->LocalAddress, sizeof(Listener->LocalAddress));
        Listener->WildCard = TRUE;
        PortUnspecified = TRUE;
    }

    //
    // Listeners always grab the dual-mode wildcard binding for the specified
    // (if available) UDP port and then manually filter on the specific address
    // (if available) at the QUIC layer.
    //
    QuicAddrSetFamily(&BindingLocalAddress, AF_INET6);
    QuicAddrSetPort(&BindingLocalAddress,
        PortUnspecified ? 0 : QuicAddrGetPort(LocalAddress));

    QuicLibraryOnListenerRegistered(Listener);

    QUIC_TEL_ASSERT(Listener->Binding == NULL);
    Status =
        QuicLibraryGetBinding(
            Listener->Session,
            TRUE,           // Listeners always share the binding.
            &BindingLocalAddress,
            NULL,
            &Listener->Binding);
    if (QUIC_FAILED(Status)) {
        EventWriteQuicListenerErrorStatus(Listener, Status, "Get binding");
        goto Error;
    }

    QuicRundownReInitialize(&Listener->Rundown);

    if (!QuicBindingRegisterListener(Listener->Binding, Listener)) {
        EventWriteQuicListenerError(Listener, "Register with binding");
        QuicRundownRelease(&Listener->Rundown);
        Status = QUIC_STATUS_INVALID_STATE;
        goto Error;
    }

    if (PortUnspecified) {
        QuicDataPathBindingGetLocalAddress(
            Listener->Binding->DatapathBinding,
            &BindingLocalAddress);
        QuicAddrSetPort(
            &Listener->LocalAddress,
            QuicAddrGetPort(&BindingLocalAddress));
    }

    EventWriteQuicListenerStarted(
        Listener,
        Listener->Binding,
        LOG_ADDR_LEN(Listener->LocalAddress),
        (uint8_t*)&Listener->LocalAddress);

Error:

    if (QUIC_FAILED(Status)) {
        if (Listener->Binding != NULL) {
            QuicLibraryReleaseBinding(Listener->Binding);
            Listener->Binding = NULL;
        }
    }

Exit:

    EventWriteQuicApiExitStatus(Status);

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
void
QUIC_API
MsQuicListenerStop(
    _In_ _Pre_defensive_ HQUIC Handle
    )
{
    EventWriteQuicApiEnter(
        QUIC_TRACE_API_LISTENER_STOP,
        Handle);

    if (Handle != NULL && Handle->Type == QUIC_HANDLE_TYPE_LISTENER) {
#pragma prefast(suppress: __WARNING_25024, "Pointer cast already validated.")
        PQUIC_LISTENER Listener = (PQUIC_LISTENER)Handle;
        if (Listener->Binding != NULL) {
            QuicBindingUnregisterListener(Listener->Binding, Listener);
            QuicLibraryReleaseBinding(Listener->Binding);
            Listener->Binding = NULL;

            QuicRundownReleaseAndWait(&Listener->Rundown);
            EventWriteQuicListenerStopped(Listener);
        }
    }

    EventWriteQuicApiExit();
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicListenerTraceRundown(
    _In_ PQUIC_LISTENER Listener
    )
{
    EventWriteQuicListenerRundown(Listener, Listener->Session);
    if (Listener->Binding != NULL) {
        EventWriteQuicListenerStarted(
            Listener,
            Listener->Binding,
            LOG_ADDR_LEN(Listener->LocalAddress),
            (uint8_t*)&Listener->LocalAddress);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicListenerIndicateEvent(
    _In_ PQUIC_LISTENER Listener,
    _Inout_ QUIC_LISTENER_EVENT* Event
    )
{
    QUIC_FRE_ASSERT(Listener->ClientCallbackHandler);
    uint64_t StartTime = QuicTimeUs64();
    QUIC_STATUS Status =
        Listener->ClientCallbackHandler(
            (HQUIC)Listener,
            Listener->ClientContext,
            Event);
    uint64_t EndTime = QuicTimeUs64();
    if (EndTime - StartTime > QUIC_MAX_CALLBACK_TIME_WARNING) {
        LogWarning("[list][%p] App took excessive time (%llu us) in callback.",
            Listener, (EndTime - StartTime));
        QUIC_TEL_ASSERTMSG_ARGS(
            EndTime - StartTime < QUIC_MAX_CALLBACK_TIME_ERROR,
            "App extremely long time in listener callback",
            Listener->Session->Registration->AppName,
            Event->Type, 0);
    }
    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicListenerClaimConnection(
    _In_ PQUIC_LISTENER Listener,
    _In_ PQUIC_CONNECTION Connection,
    _In_ const QUIC_NEW_CONNECTION_INFO* Info,
    _Out_ QUIC_SEC_CONFIG** SecConfig
    )
{
    QUIC_DBG_ASSERT(Listener != NULL);
    QUIC_DBG_ASSERT(Connection->State.ExternalOwner == FALSE);

    //
    // Internally, the connection matches the listener. Update the associated
    // connection state. Next, call up to the application layer to accept the
    // connection and return the server certificate.
    //

    QuicSessionRegisterConnection(Listener->Session, Connection);

    QUIC_LISTENER_EVENT Event;
    Event.Type = QUIC_LISTENER_EVENT_NEW_CONNECTION;
    Event.NEW_CONNECTION.Info = Info;
    Event.NEW_CONNECTION.Connection = (HQUIC)Connection;
    Event.NEW_CONNECTION.SecurityConfig = NULL;

    QuicSessionAttachSilo(Listener->Session);

    LogVerbose("[list][%p] Indicating NEW_CONNECTION", Listener);
    QUIC_STATUS Status = QuicListenerIndicateEvent(Listener, &Event);

    QuicSessionDetachSilo();

    if (Status == QUIC_STATUS_PENDING) {
        LogVerbose("[list][%p] App indicate pending NEW_CONNECTION", Listener);
        QUIC_DBG_ASSERT(Event.NEW_CONNECTION.SecurityConfig == NULL);
        *SecConfig = NULL;
    } else if (QUIC_FAILED(Status)) {
        EventWriteQuicListenerErrorStatus(Listener, Status, "NEW_CONNECTION callback");
        goto Exit;
    } else if (Event.NEW_CONNECTION.SecurityConfig == NULL) {
        EventWriteQuicListenerError(Listener, "NEW_CONNECTION callback didn't set SecConfig");
        Status = QUIC_STATUS_INVALID_PARAMETER;
        goto Exit;
    } else {
        LogVerbose("[list][%p] App accepted NEW_CONNECTION", Listener);
        *SecConfig = Event.NEW_CONNECTION.SecurityConfig;
    }

    //
    // The application layer has accepted the connection and provided a
    // server certificate.
    //
    QUIC_FRE_ASSERTMSG(
        Connection->ClientCallbackHandler != NULL,
        "App MUST set callback handler!");

    Connection->State.ExternalOwner = TRUE;
    Connection->State.ListenerAccepted = TRUE;

    if (!QuicConnGenerateNewSourceCid(Connection, TRUE)) {
        Event.NEW_CONNECTION.SecurityConfig = NULL;
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Exit;
    }

    if (Event.NEW_CONNECTION.SecurityConfig != NULL) {
        (void)QuicTlsSecConfigAddRef(Event.NEW_CONNECTION.SecurityConfig);
    }
    Connection->State.UpdateWorker = TRUE;

Exit:

    if (Status != QUIC_STATUS_PENDING && QUIC_FAILED(Status)) {
        QuicSessionUnregisterConnection(Connection->Session, Connection);
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_CONNECTION_ACCEPT_RESULT
QuicListenerAcceptConnection(
    _In_ PQUIC_LISTENER Listener,
    _In_ PQUIC_CONNECTION Connection,
    _In_ const QUIC_NEW_CONNECTION_INFO* Info
    )
{
    QUIC_STATUS Status;
    QUIC_SEC_CONFIG* SecConfig = NULL;

    QUIC_CONNECTION_ACCEPT_RESULT AcceptResult =
        QuicRegistrationAcceptConnection(
            Listener->Session->Registration,
            Connection);
    if (AcceptResult != QUIC_CONNECTION_ACCEPT) {
        QuicRundownRelease(&Listener->Rundown);
        goto Error;
    }

    AcceptResult = QUIC_CONNECTION_REJECT_APP;
    Status =
        QuicListenerClaimConnection(
            Listener,
            Connection,
            Info,
            &SecConfig);
    QuicRundownRelease(&Listener->Rundown);

    if (QUIC_FAILED(Status) && Status != QUIC_STATUS_PENDING) {
        QUIC_TEL_ASSERTMSG(SecConfig == NULL, "App failed AND provided a sec config?");
        goto Error;
    }

    if (SecConfig == NULL) {
        if (Status != QUIC_STATUS_PENDING) {
            LogVerbose("[conn][%p] No security config was provided by the app.", Connection);
            goto Error;
        }
    }

    Connection->Stats.Timing.Start = QuicTimeUs64();
    EventWriteQuicConnHandshakeStart(Connection);

    if (Status != QUIC_STATUS_PENDING) {
        Status = QuicConnHandshakeConfigure(Connection, SecConfig);
        if (QUIC_FAILED(Status)) {
            goto Error;
        }
    }

    AcceptResult = QUIC_CONNECTION_ACCEPT;

Error:

    return AcceptResult;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicListenerParamSet(
    _In_ PQUIC_LISTENER Listener,
    _In_ uint32_t Param,
    _In_ uint32_t BufferLength,
    _In_reads_bytes_(BufferLength)
        const void* Buffer
    )
{
    QUIC_STATUS Status;

    UNREFERENCED_PARAMETER(Listener);
    UNREFERENCED_PARAMETER(BufferLength);
    UNREFERENCED_PARAMETER(Buffer);

    switch (Param) {

    default:
        Status = QUIC_STATUS_INVALID_PARAMETER;
        break;
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
QuicListenerParamGet(
    _In_ PQUIC_LISTENER Listener,
    _In_ uint32_t Param,
    _Inout_ uint32_t* BufferLength,
    _Out_writes_bytes_opt_(*BufferLength)
        void* Buffer
    )
{
    QUIC_STATUS Status;

    switch (Param) {

    case QUIC_PARAM_LISTENER_LOCAL_ADDRESS:

        if (*BufferLength < sizeof(QUIC_ADDR)) {
            *BufferLength = sizeof(QUIC_ADDR);
            Status = QUIC_STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (Buffer == NULL) {
            Status = QUIC_STATUS_INVALID_PARAMETER;
            break;
        }

        *BufferLength = sizeof(QUIC_ADDR);
        QuicCopyMemory(Buffer, &Listener->LocalAddress, sizeof(QUIC_ADDR));

        Status = QUIC_STATUS_SUCCESS;
        break;

    default:
        Status = QUIC_STATUS_INVALID_PARAMETER;
        break;
    }

    return Status;
}