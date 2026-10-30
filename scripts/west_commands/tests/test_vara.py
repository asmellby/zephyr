# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
from unittest.mock import call, patch

from conftest import RC_KERNEL_ELF

from runners.vara import VaraBinaryRunner

os_path_isfile = os.path.isfile


def os_path_isfile_patch(filename):
    if filename == RC_KERNEL_ELF:
        return True
    return os_path_isfile(filename)


def create_runner(runner_config, args):
    parser = argparse.ArgumentParser(allow_abbrev=False)
    VaraBinaryRunner.add_parser(parser)
    return VaraBinaryRunner.create(runner_config, parser.parse_args(args))


@patch('runners.core.ZephyrBinaryRunner.require', side_effect=lambda program: program)
@patch('runners.core.ZephyrBinaryRunner.check_call')
def test_vara_board_name(cc, req, runner_config):
    runner = create_runner(runner_config, ['--vara-board', 'xg24_rb4186c'])
    with patch('os.path.isfile', side_effect=os_path_isfile_patch):
        runner.run('simulate')
    assert cc.call_args_list == [call(['vara', 'run', '--board', 'xg24_rb4186c', RC_KERNEL_ELF])]


@patch('runners.core.ZephyrBinaryRunner.require', side_effect=lambda program: program)
@patch('runners.core.ZephyrBinaryRunner.check_call')
def test_vara_simulate_args(cc, req, runner_config):
    args = [
        '--vara-board',
        '/boards/custom.vara.toml',
        '--vara',
        '/opt/vara/bin/vara',
        '--gdb-port',
        '1234',
        '--vara-arg=--time',
        '--vara-arg=3s',
    ]
    runner = create_runner(runner_config, args)
    with patch('os.path.isfile', side_effect=os_path_isfile_patch):
        runner.run('simulate')
    expected = [
        '/opt/vara/bin/vara',
        'run',
        '--board',
        '/boards/custom.vara.toml',
        '--time',
        '3s',
        RC_KERNEL_ELF,
    ]
    assert cc.call_args_list == [call(expected)]


@patch('runners.core.ZephyrBinaryRunner.require', side_effect=lambda program: program)
@patch('runners.core.ZephyrBinaryRunner.check_call')
def test_vara_debugserver(cc, req, runner_config):
    args = [
        '--vara-board',
        'xg24_rb4186c',
        '--gdb-port',
        '1234',
        '--vara-arg=--halt-time',
        '--vara-arg=realtime',
    ]
    runner = create_runner(runner_config, args)
    with patch('os.path.isfile', side_effect=os_path_isfile_patch):
        runner.run('debugserver')
    expected = [
        'vara',
        'debugserver',
        '--board',
        'xg24_rb4186c',
        '--port',
        '1234',
        '--halt-time',
        'realtime',
        RC_KERNEL_ELF,
    ]
    assert cc.call_args_list == [call(expected)]


@patch('runners.core.ZephyrBinaryRunner.require', side_effect=lambda program: program)
@patch('runners.core.ZephyrBinaryRunner.check_call_ignore_sigint')
def test_vara_debug(cc, req, runner_config):
    runner = create_runner(runner_config, ['--vara-board', 'xg24_rb4186c'])
    with patch('os.path.isfile', side_effect=os_path_isfile_patch):
        runner.run('debug')
    assert cc.call_count == 1
    assert cc.call_args.args[0] == [
        'vara',
        'debug',
        '--board',
        'xg24_rb4186c',
        '--port',
        '3333',
        RC_KERNEL_ELF,
    ]
    assert cc.call_args.kwargs['env']['GDB'] == runner_config.gdb
