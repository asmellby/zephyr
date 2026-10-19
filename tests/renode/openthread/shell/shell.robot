# SPDX-License-Identifier: Apache-2.0

*** Settings ***
Resource                      ${CURDIR}/../common.robot
Test Timeout                  15 minutes

*** Test Cases ***
Second Node Joins The Thread Network Of The First And They Exchange Packets
    Create Thread Medium
    ${leader}=                Create Node  leader  ${REQUIRED_ELF_0}
    ${joiner}=                Create Node  joiner  ${REQUIRED_ELF_0}
    Wait For Prompt On Uart   uart:~$  testerId=${leader}  timeout=10
    Wait For Prompt On Uart   uart:~$  testerId=${joiner}  timeout=10

    Configure Network         ${leader}  0000000000000001
    Wait For State            ${leader}  leader

    Configure Network         ${joiner}  0000000000000002
    Wait For State            ${joiner}  child

    # Ping each other through the realm-local all-nodes address
    Write Line To Uart        ot ping ff03::1  testerId=${leader}
    Wait For Line On Uart     1 packets transmitted, 1 packets received  testerId=${leader}  timeout=10
    Write Line To Uart        ot ping ff03::1  testerId=${joiner}
    Wait For Line On Uart     1 packets transmitted, 1 packets received  testerId=${joiner}  timeout=10

    # UDP both ways
    Run OT Command            ${leader}  udp open
    Run OT Command            ${leader}  udp bind :: 1234
    Run OT Command            ${joiner}  udp open
    Run OT Command            ${joiner}  udp bind :: 1234
    Run OT Command            ${leader}  udp send ff03::1 1234 hello-from-leader
    Wait For Line On Uart     hello-from-leader  testerId=${joiner}  timeout=10
    Run OT Command            ${joiner}  udp send ff03::1 1234 hello-from-joiner
    Wait For Line On Uart     hello-from-joiner  testerId=${leader}  timeout=10
