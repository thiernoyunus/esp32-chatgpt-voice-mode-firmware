#!/usr/bin/env python3

from pathlib import Path
from tempfile import TemporaryDirectory

import monitor


def main():
    with TemporaryDirectory() as temporary_directory:
        monitor.LOG = Path(temporary_directory) / 'device.log'
        monitor.ROTATED_LOG = Path(temporary_directory) / 'device.log.1'
        monitor.LOG.write_text(
            '\n'.join([
                'I (1000) CodexVoice: received=3 audio_frames=0 bytes=3 rate=16000',
                'I (1001) AudioService: played=1 peak=12000 backlog=17',
                'I (1050) CodexVoice: received=243 rate=16000',
                'I (1060) AudioService: peak= backlog=',
                'I (1100) Application: << Keepalive reply',
                'I (4000) CodexVoice: received=7 audio_frames=4 bytes=184 rate=16000',
                'I (4001) AudioService: played=2 peak=12000 backlog=2',
                'I (4050) CodexVoice: received=12 audio_frames=9 bytes=72 rate=16000',
                'I (4100) Application: << Real audio reply',
            ])
        )

        data = monitor.build()
        assert data['turns'][0]['spoken'] is False
        assert data['turns'][0]['audio_frames'] == 0
        assert data['turns'][1]['spoken'] is True
        assert data['turns'][1]['audio_frames'] == 9
        assert data['stats']['audio_frames'] == 9
        assert data['stats']['max_backlog'] == 2
        assert data['stats']['silent'] == 1

        monitor.ROTATED_LOG.write_text(
            'I (5000) CodexVoice: received=8 bytes=72 rate=16000\n'
        )
        monitor.LOG.write_text('I (5100) Application: << Rotated audio reply\n')
        data = monitor.build()
        assert data['turns'][0]['text'] == 'Rotated audio reply'
        assert data['turns'][0]['audio_frames'] == 8

        monitor.ROTATED_LOG.write_text('I (10900) Application: << Old boot reply\n')
        monitor.LOG.write_text('I (10010) Application: << Current boot reply\n')
        data = monitor.build()
        assert [turn['text'] for turn in data['turns']] == ['Current boot reply']


if __name__ == '__main__':
    main()
