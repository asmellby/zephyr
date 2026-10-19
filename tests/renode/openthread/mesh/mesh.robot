# SPDX-License-Identifier: Apache-2.0

*** Settings ***
Library                       Collections
Resource                      ${CURDIR}/../common.robot
Test Timeout                  20 minutes

*** Variables ***
${NODES}                      6
# Nodes stand ${SPACING} apart in a line and reach only radios within ${RANGE}: each hears its
# neighbours, so traffic between the ends crosses every node in between.
${SPACING}                    10
${RANGE}                      15

*** Test Cases ***
Nodes In A Line Form A Mesh That Routes Packets End To End
    Create Thread Medium
    Limit Radio Range         ${RANGE}
    @{testers}=               Create List
    FOR  ${i}  IN RANGE  ${NODES}
        ${tester}=            Create Node  node${i}  ${REQUIRED_ELF_0}
        Place Node            node${i}  ${{ ${i} * ${SPACING} }}
        Append To List        ${testers}  ${tester}
    END
    FOR  ${tester}  IN  @{testers}
        Wait For Prompt On Uart  uart:~$  testerId=${tester}  timeout=10
    END

    # Become a router within a second of attaching rather than after the default 120 s jitter
    FOR  ${tester}  IN  @{testers}
        Run OT Command        ${tester}  routerselectionjitter 1
    END

    # The first node forms the network; each next one attaches to its predecessor, the only router
    # in range, and becomes a router itself so that the following node can attach to it
    Configure Network         ${testers}[0]  0000000000000001
    Wait For State            ${testers}[0]  leader
    FOR  ${i}  IN RANGE  1  ${NODES}
        Configure Network     ${testers}[${i}]  ${{ '%016x' % (${i} + 1) }}
        Wait For State        ${testers}[${i}]  router
    END

    # The first node reaches the far end only through a router in between
    Wait Until Keyword Succeeds  30x  0s  Routes Over Several Hops  ${testers}[0]  ${{ '%016x' % ${NODES} }}

    ${last}=                  Evaluate  ${NODES} - 1
    Write Line To Uart        ot ipaddr mleid  testerId=${testers}[${last}]
    ${result}=                Wait For Line On Uart  ^fd00:db8:a0:0:  testerId=${testers}[${last}]  treatAsRegex=true  timeout=10
    ${address}=               Evaluate  $result.Line.strip()
    Wait Until Keyword Succeeds  5x  0s  Ping Succeeds  ${testers}[0]  ${address}

*** Keywords ***
Routes Over Several Hops
    [Documentation]  Checks that the router table has a route to ${extaddr} that is not a direct link and
    ...              costs more than one hop.
    [Arguments]               ${tester}  ${extaddr}
    Write Line To Uart        ot router table  testerId=${tester}
    Wait For Line On Uart     ^\\|\\s*\\d+ \\| 0x[0-9a-f]{4} \\|\\s*(?!63 )\\d+ \\|\\s*([2-9]|\\d{2,}) \\|.*\\| ${extaddr} \\|\\s*0 \\|$
    ...                       testerId=${tester}  treatAsRegex=true  timeout=2

Ping Succeeds
    [Arguments]               ${tester}  ${address}
    Write Line To Uart        ot ping ${address}  testerId=${tester}
    Wait For Line On Uart     1 packets transmitted, 1 packets received  testerId=${tester}  timeout=10
