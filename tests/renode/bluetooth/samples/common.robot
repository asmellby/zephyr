# SPDX-License-Identifier: Apache-2.0

*** Keywords ***
Create Bluetooth Medium
    Execute Command           emulation CreateBLEMedium "wireless"

Create Node
    [Documentation]           Creates a machine named ${name} running ${elf} on the board of ${RESC},
    ...                       with its radio on the Bluetooth medium, and returns a tester for its console.
    [Arguments]               ${name}  ${elf}
    # Set the variables globally: set with a machine selected, they would only apply to that machine
    Execute Command           mach clear
    Execute Command           $name="${name}"
    Execute Command           $elf=${elf}
    Execute Command           include ${RESC}
    Execute Command           connector Connect sysbus.radio wireless
    ${tester}=                Create Terminal Tester  ${UART}  machine=${name}
    RETURN                    ${tester}
