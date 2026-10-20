/*
 * i2c_helper.h — Simple Peripheral Bus (SPB / I2C) Helper Functions for KMDF
 *
 * Implements standard I2C read and write transactions using SPB sequences
 * with full support for repeated starts, big-endian register addresses,
 * and 8-bit / 16-bit register word lengths.
 *
 * Copyright (c) 2026. Licensed under the MIT License.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <spb.h>
#include <reshub.h>



/*
 * I2cWriteRegister8 - Write 8-bit value to a 16-bit register address
 *
 * Format on I2C bus: [RegAddr_MSB, RegAddr_LSB, Value]
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
I2cWriteRegister8(
    _In_ WDFIOTARGET SpbTarget,
    _In_ UINT16 Register,
    _In_ UINT8 Value
    )
{
    NTSTATUS status;
    UINT8 buffer[3];
    WDF_MEMORY_DESCRIPTOR memDesc;

    buffer[0] = (UINT8)((Register >> 8) & 0xFF);
    buffer[1] = (UINT8)(Register & 0xFF);
    buffer[2] = Value;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDesc, buffer, sizeof(buffer));

    status = WdfIoTargetSendWriteSynchronously(
        SpbTarget,
        NULL,
        &memDesc,
        NULL,
        NULL,
        NULL
    );

    return status;
}

/*
 * I2cReadRegister8 - Read 8-bit value from a 16-bit register address using Repeated-Start
 *
 * Sequence:
 *   1. Write: [RegAddr_MSB, RegAddr_LSB]
 *   2. Read:  [Value]
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
I2cReadRegister8(
    _In_ WDFIOTARGET SpbTarget,
    _In_ UINT16 Register,
    _Out_ UINT8* Value
    )
{
    NTSTATUS status;
    UINT8 addrBuf[2];
    UINT8 dataBuf[1];

    SPB_TRANSFER_LIST_AND_ENTRIES(2) sequence;
    WDF_MEMORY_DESCRIPTOR inputDesc;

    if (!Value) return STATUS_INVALID_PARAMETER;
    *Value = 0;

    addrBuf[0] = (UINT8)((Register >> 8) & 0xFF);
    addrBuf[1] = (UINT8)(Register & 0xFF);

    SPB_TRANSFER_LIST_INIT(&(sequence.List), 2);

    /* Transfer 1: Write register address */
    sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionToDevice,
        0,
        addrBuf,
        sizeof(addrBuf)
    );

    /* Transfer 2: Read data byte */
    sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionFromDevice,
        0,
        dataBuf,
        sizeof(dataBuf)
    );

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDesc, &sequence, sizeof(sequence));

    status = WdfIoTargetSendIoctlSynchronously(
        SpbTarget,
        NULL,
        IOCTL_SPB_EXECUTE_SEQUENCE,
        &inputDesc,
        NULL,
        NULL,
        NULL
    );

    if (NT_SUCCESS(status)) {
        *Value = dataBuf[0];
    }

    return status;
}

/*
 * I2cWriteRegister16 - Write 16-bit value to a 16-bit register address (big-endian)
 *
 * Format on I2C bus: [RegAddr_MSB, RegAddr_LSB, Value_MSB, Value_LSB]
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
I2cWriteRegister16(
    _In_ WDFIOTARGET SpbTarget,
    _In_ UINT16 Register,
    _In_ UINT16 Value
    )
{
    NTSTATUS status;
    UINT8 buffer[4];
    WDF_MEMORY_DESCRIPTOR memDesc;

    buffer[0] = (UINT8)((Register >> 8) & 0xFF);
    buffer[1] = (UINT8)(Register & 0xFF);
    buffer[2] = (UINT8)((Value >> 8) & 0xFF);
    buffer[3] = (UINT8)(Value & 0xFF);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDesc, buffer, sizeof(buffer));

    status = WdfIoTargetSendWriteSynchronously(
        SpbTarget,
        NULL,
        &memDesc,
        NULL,
        NULL,
        NULL
    );

    return status;
}

/*
 * I2cReadRegister16 - Read 16-bit value from a 16-bit register address using Repeated-Start
 *
 * Sequence:
 *   1. Write: [RegAddr_MSB, RegAddr_LSB]
 *   2. Read:  [Value_MSB, Value_LSB]
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
I2cReadRegister16(
    _In_ WDFIOTARGET SpbTarget,
    _In_ UINT16 Register,
    _Out_ UINT16* Value
    )
{
    NTSTATUS status;
    UINT8 addrBuf[2];
    UINT8 dataBuf[2];

    SPB_TRANSFER_LIST_AND_ENTRIES(2) sequence;
    WDF_MEMORY_DESCRIPTOR inputDesc;

    if (!Value) return STATUS_INVALID_PARAMETER;
    *Value = 0;

    addrBuf[0] = (UINT8)((Register >> 8) & 0xFF);
    addrBuf[1] = (UINT8)(Register & 0xFF);

    SPB_TRANSFER_LIST_INIT(&(sequence.List), 2);

    /* Transfer 1: Write register address */
    sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionToDevice,
        0,
        addrBuf,
        sizeof(addrBuf)
    );

    /* Transfer 2: Read data bytes (MSB then LSB) */
    sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
        SpbTransferDirectionFromDevice,
        0,
        dataBuf,
        sizeof(dataBuf)
    );

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDesc, &sequence, sizeof(sequence));

    status = WdfIoTargetSendIoctlSynchronously(
        SpbTarget,
        NULL,
        IOCTL_SPB_EXECUTE_SEQUENCE,
        &inputDesc,
        NULL,
        NULL,
        NULL
    );

    if (NT_SUCCESS(status)) {
        *Value = (UINT16)((dataBuf[0] << 8) | dataBuf[1]);
    }

    return status;
}

/*
 * I2cUpdateBits8 - Read-Modify-Write for 8-bit register
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
I2cUpdateBits8(
    _In_ WDFIOTARGET SpbTarget,
    _In_ UINT16 Register,
    _In_ UINT8 Mask,
    _In_ UINT8 Value
    )
{
    NTSTATUS status;
    UINT8 currentVal = 0;
    UINT8 newVal;

    status = I2cReadRegister8(SpbTarget, Register, &currentVal);
    if (!NT_SUCCESS(status)) return status;

    newVal = (currentVal & ~Mask) | (Value & Mask);
    if (newVal == currentVal) {
        return STATUS_SUCCESS; /* No change needed */
    }

    return I2cWriteRegister8(SpbTarget, Register, newVal);
}

/*
 * I2cUpdateBits16 - Read-Modify-Write for 16-bit register
 */
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
I2cUpdateBits16(
    _In_ WDFIOTARGET SpbTarget,
    _In_ UINT16 Register,
    _In_ UINT16 Mask,
    _In_ UINT16 Value
    )
{
    NTSTATUS status;
    UINT16 currentVal = 0;
    UINT16 newVal;

    status = I2cReadRegister16(SpbTarget, Register, &currentVal);
    if (!NT_SUCCESS(status)) return status;

    newVal = (currentVal & ~Mask) | (Value & Mask);
    if (newVal == currentVal) {
        return STATUS_SUCCESS; /* No change needed */
    }

    return I2cWriteRegister16(SpbTarget, Register, newVal);
}
