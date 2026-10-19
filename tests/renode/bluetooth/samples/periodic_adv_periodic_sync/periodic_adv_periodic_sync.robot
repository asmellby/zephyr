# SPDX-License-Identifier: Apache-2.0

*** Settings ***
Resource                      ${CURDIR}/../common.robot
Test Timeout                  15 minutes

*** Test Cases ***
Periodic Sync Synchronizes To Periodic Advertising
    Create Bluetooth Medium
    ${advertiser}=            Create Node  advertiser  ${REQUIRED_ELF_0}
    ${scanner}=               Create Node  scanner  ${REQUIRED_ELF_1}

    Wait For Line On Uart     Start Extended Advertising...done.  testerId=${advertiser}  timeout=10
    Wait For Line On Uart     Found periodic advertising.  testerId=${scanner}  timeout=30
    Wait For Line On Uart     synced  testerId=${scanner}  timeout=30
    Wait For Line On Uart     data length  testerId=${scanner}  timeout=30
