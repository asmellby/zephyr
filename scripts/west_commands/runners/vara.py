# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

'''Runner for the vara SoC emulator.'''

from runners.core import RunnerCaps, ZephyrBinaryRunner


class VaraBinaryRunner(ZephyrBinaryRunner):
    '''Runs zephyr.elf on a board emulated by vara.'''

    def __init__(self, cfg, board, vara='vara', gdb_port=None, vara_args=None):
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
        return RunnerCaps(commands={'simulate'}, hide_load_files=True)

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument(
            '--vara-board',
            required=True,
            help='vara board name, or path to a vara board .toml file',
        )
        parser.add_argument('--vara', default='vara', help='vara executable, default is "vara"')
        parser.add_argument(
            '--gdb-port', metavar='PORT', help='start halted with a GDB server on PORT'
        )
        parser.add_argument(
            '--vara-arg',
            dest='vara_args',
            metavar='ARG',
            action='append',
            help='additional argument passed to "vara run", may be repeated',
        )

    @classmethod
    def do_create(cls, cfg, args):
        return VaraBinaryRunner(
            cfg, args.vara_board, vara=args.vara, gdb_port=args.gdb_port, vara_args=args.vara_args
        )

    def do_run(self, command, **kwargs):
        self.ensure_output('elf')
        vara = self.require(self.vara)

        cmd = [vara, 'run', '--board', self.board]
        if self.gdb_port is not None:
            cmd += ['--gdb', str(self.gdb_port)]
        cmd += self.vara_args
        cmd.append(self.cfg.elf_file)

        self.check_call(cmd)
