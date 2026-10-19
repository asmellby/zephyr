# SPDX-License-Identifier: Apache-2.0

*** Settings ***
Resource                      ${CURDIR}/../common.robot
Test Timeout                  15 minutes

*** Test Cases ***
Central HT Receives Temperature Indications From Peripheral HT
    Create Bluetooth Medium
    ${central}=               Create Node  central  ${REQUIRED_ELF_0}
    ${peripheral}=            Create Node  peripheral  ${REQUIRED_ELF_1}

    Wait For Line On Uart     Advertising successfully started  testerId=${peripheral}  timeout=10
    Wait For Line On Uart     Scanning successfully started  testerId=${central}  timeout=10
    Wait For Line On Uart     Connected  testerId=${peripheral}  timeout=30
    Wait For Line On Uart     [SUBSCRIBED]  testerId=${central}  timeout=30
    Wait For Line On Uart     Indication success  testerId=${peripheral}  timeout=30
    Wait For Line On Uart     Temperature  testerId=${central}  timeout=30
