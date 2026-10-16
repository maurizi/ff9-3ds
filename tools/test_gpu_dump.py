#!/usr/bin/env python3
"""Disc-free checks for captured-command boundaries and replay cancellation."""
import unittest
from replay_gpu_dump import MAGIC, command_length, normalize, words

def packet(kind,payload=()):return words([(kind<<24)|len(payload),*payload])

class DumpTests(unittest.TestCase):
    def test_upload_split_across_packets_and_control(self):
        # Three pixels need two data words, including the unused final half.
        data=(MAGIC+packet(0,[0xa0000000,0,0x10003])+packet(1,[0x05000140])+
              packet(0,[0x22221111,0x3333])+packet(5)+packet(0x7f,[1,2])+packet(2,[1,0]))
        records,writes,meta=normalize(data,1)
        self.assertEqual(records,[(1,[0x05000140]),(0,[0xa0000000,0,0x10003,0x22221111,0x3333])])
        self.assertEqual(len(writes),6)
        self.assertEqual(meta['display_origin'],[320,0])

    def test_cancel_partial_command(self):
        data=(MAGIC+packet(5)+packet(0,[0x28000000,0])+packet(1,[0x01000000])+
              packet(0,[0xe1000200])+packet(2))
        records,_,meta=normalize(data,1)
        self.assertEqual(records,[(1,[0x01000000]),(0,[0xe1000200])])
        self.assertEqual(meta['cancelled_words'],2)

    def test_polyline_terminator_parameter_alignment(self):
        self.assertEqual(command_length([0x48000000,0x50005000,1,0x50005000]),4)
        self.assertIsNone(command_length([0x58000000,0,1,2,3,0x50005000]))
        self.assertEqual(command_length([0x58000000,0,1,2,0x50005000]),5)

    def test_discarded_words_are_not_commands(self):
        records,writes,_=normalize(MAGIC+packet(3,[0x28000000])+packet(5),0)
        self.assertEqual((records,writes),([],[]))

    def test_reject_incomplete_and_unsupported_streams(self):
        for data in [b'bad magic',MAGIC+b'\0',MAGIC+words([10]),
                     MAGIC+packet(5)+packet(0,[0x28000000])+packet(2),
                     MAGIC+packet(5)+packet(4,[1]),
                     MAGIC+packet(1,[0x08000020])+packet(5),
                     MAGIC+packet(5)+packet(0,[0xc0000000,0,0x10001])]:
            with self.subTest(data=data),self.assertRaises(ValueError):normalize(data,1)

if __name__=='__main__':unittest.main()
