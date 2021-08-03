//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

#pragma once

EXTERN_C_START

//
// Initializes a GUID.
//
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
XdpGuidCreate(
    _Out_ GUID *Guid
    );

EXTERN_C_END