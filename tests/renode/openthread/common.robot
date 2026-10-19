# SPDX-License-Identifier: Apache-2.0

*** Keywords ***
Create Thread Medium
    Execute Command           emulation CreateIEEE802_15_4Medium "wireless"

Create Node
    [Documentation]           Creates a machine named ${name} running ${elf} on the board of ${RESC},
    ...                       with its radio on the IEEE 802.15.4 medium, and returns a tester for its console.
    [Arguments]               ${name}  ${elf}
    # Set the variables globally: set with a machine selected, they would only apply to that machine
    Execute Command           mach clear
    Execute Command           $name="${name}"
    Execute Command           $elf=${elf}
    Execute Command           include ${RESC}
    Execute Command           connector Connect sysbus.radio wireless
    ${tester}=                Create Terminal Tester  ${UART}  machine=${name}
    RETURN                    ${tester}

Run OT Command
    [Arguments]               ${tester}  ${command}
    Write Line To Uart        ot ${command}  testerId=${tester}
    Wait For Line On Uart     Done  testerId=${tester}  timeout=10

Configure Network
    [Documentation]           Gives the node an extended address of its own and the test network's dataset.
    [Arguments]               ${tester}  ${extaddr}
    Run OT Command            ${tester}  extaddr ${extaddr}
    Run OT Command            ${tester}  dataset init new
    Run OT Command            ${tester}  dataset activetimestamp 1
    Run OT Command            ${tester}  dataset networkname renode
    Run OT Command            ${tester}  dataset networkkey 00112233445566778899aabbccddeeff
    Run OT Command            ${tester}  dataset extpanid dead00beef00cafe
    Run OT Command            ${tester}  dataset panid 0x1234
    Run OT Command            ${tester}  dataset channel 15
    Run OT Command            ${tester}  dataset meshlocalprefix fd00:db8:a0::
    Run OT Command            ${tester}  dataset commit active
    Run OT Command            ${tester}  ifconfig up
    Run OT Command            ${tester}  thread start

Node Has State
    [Arguments]               ${tester}  ${state}
    Write Line To Uart        ot state  testerId=${tester}
    Wait For Line On Uart     ${state}  testerId=${tester}  timeout=1

Wait For State
    [Documentation]           Polls the node's Thread role until it is ${state}.
    [Arguments]               ${tester}  ${state}
    Wait Until Keyword Succeeds  60x  0s  Node Has State  ${tester}  ${state}
