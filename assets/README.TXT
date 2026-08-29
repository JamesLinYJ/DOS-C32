DOS-C32 32-bit bring-up image

This is a protected-mode engineering image, not yet a complete DOS.
The current shell can read a FAT16 volume and implements these COMMAND.COM
operations: DIR, TYPE, CD/CHDIR, ECHO, REM, CLS, VER, VOL, and EXIT.

The compatibility plan includes the MS-DOS service personality,
16-bit COM/MZ execution in a protected compatibility environment, XMS/EMS and
INT 2Fh compatibility, and unmodified Windows 3.2 as a final acceptance target.

Implementation plan: PLAN.md
