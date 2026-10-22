#!/usr/bin/env python3
# Copyright (c) 2026 Silicon Laboratories Inc.
# SPDX-License-Identifier: Apache-2.0

"""Run a Zephyr application in a Renode emulation shared with other applications.

The first invocation starts Renode in the background with its Robot Framework
XML-RPC server on a fixed port. Later invocations, from other build directories
and possibly for other boards, find that Renode and add their own machine to the
same emulation, so the machines share virtual time and a wireless medium.

Each invocation:

- adds a machine with a unique name from the board's Renode script and the
  application's ELF,
- connects the machine's radio, if the application's .config enables
  Bluetooth LE or IEEE 802.15.4, to the wireless medium all machines share,
- routes the board's console UART to a TCP socket and bridges the terminal to
  it, so each terminal shows the console of its own machine only.

A machine added while the emulation runs starts at the current virtual time of
the emulation; its firmware sees its uptime start at zero. With --wait-for N
the emulation only starts once N machines have joined, so they all start at
virtual time zero.

Press Ctrl-] to leave. The machine is then removed, and Renode exits when the
last machine is gone.
"""

import argparse
import errno
import fcntl
import os
import select
import signal
import socket
import subprocess
import sys
import tempfile
import termios
import time
import xmlrpc.client
from contextlib import contextmanager

DEFAULT_PORT = 25810
EXIT_KEY = b'\x1d'  # Ctrl-]
RENODE_START_TIMEOUT = 60
RPC_TIMEOUT = 120

# All radios share one medium, whatever protocols their applications run: the
# radio models only deliver a frame to a radio tuned to the same carrier
# frequency with a PHY that can decode it. The medium type only selects what a
# Wireshark capture decodes the frames as.
MEDIUM = 'wireless'
MEDIUM_TYPES = {
    'ble': 'CreateBLEMedium',
    'ieee802154': 'CreateIEEE802_15_4Medium',
}


def log(msg):
    # The terminal may be in raw input mode, which keeps output processing,
    # so a plain newline is enough
    print(f'renode_multi: {msg}', file=sys.stderr, flush=True)


class RenodeError(Exception):
    pass


class TimeoutTransport(xmlrpc.client.Transport):
    def make_connection(self, host):
        conn = super().make_connection(host)
        conn.timeout = RPC_TIMEOUT
        return conn


class Renode:
    """Monitor access to a Renode instance through its Robot Framework server."""

    def __init__(self, port):
        self.proxy = xmlrpc.client.ServerProxy(
            f'http://127.0.0.1:{port}/', transport=TimeoutTransport(), allow_none=True
        )

    def _keyword(self, name, *args):
        result = self.proxy.run_keyword(name, list(args))
        if result.get('status') != 'PASS':
            raise RenodeError(result.get('error', '').strip())
        return result.get('return')

    def ping(self):
        try:
            self.python('1')
            return True
        except (OSError, xmlrpc.client.Error):
            return False

    def command(self, cmd):
        return self._keyword('ExecuteCommand', cmd)

    def python(self, code):
        return self._keyword('ExecutePython', code)

    def machine_names(self):
        names = self.python(
            'from Antmicro.Renode.Core import EmulationManager\n'
            '"\\n".join(EmulationManager.Instance.CurrentEmulation.Names)'
        )
        return [n for n in names.split('\n') if n]

    def external_names(self):
        names = self.python(
            'from Antmicro.Renode.Core import EmulationManager\n'
            '"\\n".join(EmulationManager.Instance.CurrentEmulation.ExternalsManager.GetNames())'
        )
        return [n for n in names.split('\n') if n]

    def remove_external(self, name):
        self.python(
            'from Antmicro.Renode.Core import EmulationManager\n'
            'm = EmulationManager.Instance.CurrentEmulation.ExternalsManager\n'
            'for e in list(m.Externals):\n'
            f'    if m.TryGetName(e)[1] == "{name}":\n'
            '        m.RemoveExternal(e)\n'
            '        if hasattr(e, "Dispose"):\n'
            '            e.Dispose()\n'
        )

    def is_started(self):
        return self.python(
            'from Antmicro.Renode.Core import EmulationManager\n'
            'EmulationManager.Instance.CurrentEmulation.IsStarted'
        )


class SharedRenode:
    """The Renode instance shared by all invocations using the same port."""

    def __init__(self, renode_path, port, log_level):
        self.renode_path = renode_path
        self.port = port
        self.log_level = log_level
        self.state_dir = os.path.join(
            tempfile.gettempdir(), f'zephyr-renode-multi-{os.getuid()}-{port}'
        )
        os.makedirs(self.state_dir, exist_ok=True)
        self.pid_file = os.path.join(self.state_dir, 'renode.pid')
        self.log_file = os.path.join(self.state_dir, 'renode.log')
        self.lock_file = os.path.join(self.state_dir, 'lock')
        self.renode = Renode(port)

    @contextmanager
    def lock(self):
        with open(self.lock_file, 'w') as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(f, fcntl.LOCK_UN)

    def pid(self):
        try:
            with open(self.pid_file) as f:
                pid = int(f.read().strip())
            os.kill(pid, 0)
            return pid
        except (OSError, ValueError):
            return None

    def find_or_start(self):
        """Return the running Renode, starting one if needed. Call with the lock held."""
        pid = self.pid()
        if self.renode.ping():
            if pid is None:
                raise RenodeError(
                    f'port {self.port} is used by a Renode not started by this script; '
                    'set RENODE_MULTI_PORT to use another port'
                )
            log(f'joining Renode (pid {pid}) on port {self.port}')
            return
        if pid is not None:
            raise RenodeError(
                f'Renode (pid {pid}) from {self.pid_file} does not answer on port {self.port}'
            )

        log(f'starting Renode on port {self.port}, log in {self.log_file}')
        if os.path.exists(self.pid_file):
            os.unlink(self.pid_file)
        with open(self.log_file, 'w') as log_out:
            proc = subprocess.Popen(
                [
                    self.renode_path,
                    '--disable-xwt',
                    '--port',
                    '-2',
                    '--robot-server-port',
                    str(self.port),
                    '--pid-file',
                    self.pid_file,
                ],
                stdin=subprocess.DEVNULL,
                stdout=log_out,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        deadline = time.monotonic() + RENODE_START_TIMEOUT
        while not self.renode.ping():
            if proc.poll() is not None:
                raise RenodeError(f'Renode exited with {proc.returncode}, see {self.log_file}')
            if time.monotonic() > deadline:
                proc.kill()
                raise RenodeError(f'Renode did not answer on port {self.port}')
            time.sleep(0.5)
        self.renode.command(f'logLevel {self.log_level}')

    def stop(self):
        pid = self.pid()
        if pid is None:
            return
        log(f'no machines left, stopping Renode (pid {pid})')
        os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except OSError:
                break
            time.sleep(0.2)
        else:
            os.kill(pid, signal.SIGKILL)
        if os.path.exists(self.pid_file):
            os.unlink(self.pid_file)


def medium_from_config(config_path):
    options = set()
    try:
        with open(config_path) as f:
            for line in f:
                if line.rstrip().endswith('=y'):
                    options.add(line.split('=', 1)[0])
    except OSError:
        return None
    if options & {'CONFIG_IEEE802154', 'CONFIG_OPENTHREAD'}:
        return 'ieee802154'
    if 'CONFIG_BT' in options:
        return 'ble'
    return None


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def unique_name(base, taken):
    name = base.replace('"', '').replace(' ', '_') or 'machine'
    index = 1
    candidate = name
    while candidate in taken:
        candidate = f'{name}-{index}'
        index += 1
    return candidate


class Machine:
    def __init__(self, shared, args, medium):
        self.shared = shared
        self.renode = shared.renode
        self.args = args
        self.medium = medium
        self.name = None
        self.terminal = None
        self.console = None

    def attach(self):
        """Add the machine, paused, and connect it to the medium and its console socket.

        Call with the lock held and the emulation paused.
        """
        r = self.renode
        self.name = unique_name(self.args.name, r.machine_names())
        self.terminal = f'{self.name}_console'
        console_port = free_port()

        if self.medium and MEDIUM not in r.external_names():
            r.command(f'emulation {MEDIUM_TYPES[self.medium]} "{MEDIUM}"')

        # Monitor variables set with a machine selected only apply to that machine
        r.command('mach clear')
        r.command(f'$name="{self.name}"')
        r.command(f'$elf=@{self.args.elf}')
        try:
            r.command(f'include @{self.args.resc}')
            for overlay in self.args.overlay:
                r.command(f'include @{overlay}')
            r.command(f'mach set "{self.name}"')
            if self.medium:
                r.command(f'connector Connect sysbus.radio {MEDIUM}')
            r.command(
                f'emulation CreateServerSocketTerminal {console_port} "{self.terminal}" false'
            )
            r.command(f'connector Connect {self.args.uart} {self.terminal}')
        finally:
            r.command('mach clear')

        self.console = socket.create_connection(('127.0.0.1', console_port))
        log(
            f'added machine "{self.name}" ({os.path.basename(self.args.resc)}'
            f'{", radio on the " + MEDIUM + " medium" if self.medium else ""})'
        )

    def detach(self):
        """Remove the machine and its console. Call with the lock held."""
        r = self.renode
        if self.console:
            self.console.close()
            self.console = None
        if self.name is None:
            return
        steps = []
        if self.name in r.machine_names():
            steps += [
                f'mach set "{self.name}"',
                f'connector Disconnect {self.args.uart} {self.terminal}',
            ]
            if self.medium:
                steps.append(f'connector Disconnect sysbus.radio {MEDIUM}')
            steps += ['mach clear', f'emulation RemoveMachine "{self.name}"']
        for step in steps:
            try:
                r.command(step)
            except RenodeError as e:
                log(f'{step}: {e}')
        if self.terminal in r.external_names():
            r.remove_external(self.terminal)
        r.command('mach clear')
        log(f'removed machine "{self.name}"')
        self.name = None


@contextmanager
def raw_input_mode(fd):
    """Pass every key, including Ctrl-C, to the console; keep output processing."""
    old = termios.tcgetattr(fd)
    new = termios.tcgetattr(fd)
    new[0] &= ~(termios.BRKINT | termios.ICRNL | termios.INPCK | termios.ISTRIP | termios.IXON)
    new[3] &= ~(termios.ECHO | termios.ICANON | termios.IEXTEN | termios.ISIG)
    new[6][termios.VMIN] = 1
    new[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSAFLUSH, new)
    try:
        yield
    finally:
        termios.tcsetattr(fd, termios.TCSAFLUSH, old)


class Stop(Exception):
    pass


def bridge(console):
    """Copy between the terminal and the console socket until the user leaves."""
    stdin = sys.stdin.fileno()
    stdout = sys.stdout.fileno()
    interactive = os.isatty(stdin)
    inputs = [console, stdin]

    def on_signal(signum, frame):
        raise Stop

    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, on_signal)

    if interactive:
        log('press Ctrl-] to leave')
    try:
        while True:
            try:
                ready, _, _ = select.select(inputs, [], [])
            except InterruptedError:
                continue
            if console in ready:
                data = console.recv(4096)
                if not data:
                    log('console closed by Renode')
                    return
                os.write(stdout, data)
            if stdin in ready:
                data = os.read(stdin, 1024)
                if not data:
                    # Keep showing the console until interrupted
                    inputs.remove(stdin)
                    continue
                if interactive and EXIT_KEY in data:
                    data = data[: data.index(EXIT_KEY)]
                    if data:
                        console.sendall(data)
                    return
                console.sendall(data)
    except Stop:
        return


def run(args):
    medium = args.medium
    if medium == 'auto':
        medium = medium_from_config(args.config) if args.config else None
    elif medium == 'none':
        medium = None

    shared = SharedRenode(args.renode, args.port, args.log_level)
    machine = Machine(shared, args, medium)
    r = shared.renode

    with shared.lock():
        shared.find_or_start()
        was_started = r.is_started()
        if was_started:
            r.command('pause')
        try:
            machine.attach()
        except Exception:
            machine.detach()
            if was_started:
                r.command('start')
            raise
        count = len(r.machine_names())
        if was_started or count >= args.wait_for:
            # Starting an emulation without machines would let its virtual time run away;
            # here it always has at least this machine
            r.command('start')
        else:
            log(f'waiting for {args.wait_for - count} more machine(s) to join before starting')

    interactive = os.isatty(sys.stdin.fileno())
    try:
        if interactive:
            with raw_input_mode(sys.stdin.fileno()):
                bridge(machine.console)
        else:
            bridge(machine.console)
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        with shared.lock():
            try:
                was_started = r.is_started()
                if was_started:
                    r.command('pause')
                machine.detach()
                remaining = r.machine_names()
                if remaining and was_started:
                    r.command('start')
            except (OSError, xmlrpc.client.Error, RenodeError) as e:
                log(f'Renode is gone: {e}')
                remaining = []
            if not remaining:
                shared.stop()


def env_int(name, default):
    value = os.environ.get(name)
    return int(value) if value else default


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False,
    )
    parser.add_argument('--renode', default='renode', help='Renode executable')
    parser.add_argument('--elf', required=True, help='application ELF file')
    parser.add_argument('--resc', required=True, help="board's Renode script")
    parser.add_argument('--uart', required=True, help="board's console UART, e.g. sysbus.uart0")
    parser.add_argument(
        '--overlay', action='append', default=[], help='Renode script to run after --resc'
    )
    parser.add_argument('--config', help='application .config, used to choose the medium')
    parser.add_argument(
        '--medium',
        choices=['auto', 'none', *MEDIUM_TYPES],
        default='auto',
        help='connect sysbus.radio to the shared medium, created with this type if it does '
        'not exist yet; "none" leaves the radio unconnected (default: from --config)',
    )
    parser.add_argument(
        '--name',
        help='machine name, made unique with a suffix '
        '(default: $RENODE_MULTI_NAME, else --default-name)',
    )
    parser.add_argument(
        '--default-name', help='machine name if neither --name nor $RENODE_MULTI_NAME is set'
    )
    parser.add_argument(
        '--port',
        type=int,
        default=env_int('RENODE_MULTI_PORT', DEFAULT_PORT),
        help='Robot Framework server port of the shared Renode '
        f'(default: $RENODE_MULTI_PORT, else {DEFAULT_PORT})',
    )
    parser.add_argument(
        '--wait-for',
        type=int,
        default=env_int('RENODE_MULTI_WAIT_FOR', 1),
        help='start the emulation once this many machines have joined '
        '(default: $RENODE_MULTI_WAIT_FOR, else 1)',
    )
    parser.add_argument(
        '--log-level',
        type=int,
        default=env_int('RENODE_MULTI_LOG_LEVEL', 3),
        help='Renode log level, 0 (noisy) to 3 (error) (default: $RENODE_MULTI_LOG_LEVEL, else 3)',
    )
    args = parser.parse_args(argv)
    args.name = args.name or os.environ.get('RENODE_MULTI_NAME') or args.default_name
    if not args.name:
        # <build dir>/zephyr/zephyr.elf: name the machine after the build directory
        args.name = os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(args.elf))))
    return args


def main():
    args = parse_args()
    try:
        run(args)
    except RenodeError as e:
        log(f'error: {e}')
        sys.exit(1)
    except OSError as e:
        if e.errno == errno.ECONNREFUSED:
            log('error: lost the connection to Renode')
            sys.exit(1)
        raise


if __name__ == '__main__':
    main()
