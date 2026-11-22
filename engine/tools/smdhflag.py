#!/usr/bin/env python3
"""smdhflag.py SMDH: set the icon's New 3DS only flag, 0x1000 of the flags
word at 0x2028, as a retail New 3DS title's icon has it. an Old 3DS refuses
such a title, and the HOME Menu gives it the New 3DS's memory"""
import sys

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())
data[0x2029] |= 0x10
open(path, 'wb').write(data)
