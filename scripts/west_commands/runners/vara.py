# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

'''Runner for the vara SoC emulator.'''

import os

from runners.core import RunnerCaps, ZephyrBinaryRunner


class VaraBinaryRunner(ZephyrBinaryRunner):
    '''Runs zephyr.elf on a board emulated by vara.'''

    def __init__(self, cfg, board, vara='vara', gdb_port=3333, vara_args=None):
        super().__init__(cfg)
        self.board = board
        self.vara = vara
        self.gdb_port = gdb_port
        self.vara_args = vara_args or []

    @classmethod
    def name(cls):
        return 'vara'

    @classmethod
    def capabilities(cls):
        return RunnerCaps(commands={'simulate', 'debug', 'debugserver'}, hide_load_files=True)

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument(
            '--vara-board',
            required=True,
            help='vara board name, or path to a vara board .toml file',
        )
        parser.add_argument('--vara', default='vara', help='vara executable, default is "vara"')
        parser.add_argument(
            '--gdb-port',
            metavar='PORT',
            default=3333,
            help='GDB server port for debug and debugserver, default is 3333',
        )
        parser.add_argument(
            '--vara-arg',
            dest='vara_args',
            metavar='ARG',
            action='append',
            help='additional argument passed to vara, may be repeated',
        )

    @classmethod
    def do_create(cls, cfg, args):
        return VaraBinaryRunner(
            cfg, args.vara_board, vara=args.vara, gdb_port=args.gdb_port, vara_args=args.vara_args
        )

    def do_run(self, command, **kwargs):
        self.ensure_output('elf')
        vara = self.require(self.vara)

        verb = 'run' if command == 'simulate' else command
        cmd = [vara, verb, '--board', self.board]
        if command != 'simulate':
            cmd += ['--port', str(self.gdb_port)]
        cmd += self.vara_args
        cmd.append(self.cfg.elf_file)

        if command == 'debug':
            env = os.environ.copy()
            if self.cfg.gdb:
                env['GDB'] = self.cfg.gdb
            self.check_call_ignore_sigint(cmd, env=env)
        else:
            self.check_call(cmd)
