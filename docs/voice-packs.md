# Voice packs

FF9 has no voice acting. The 3DS port can play a recording for any dialogue line, from an optional pack on the
SD card. The repository ships none: a pack is made from recordings its makers own.

## Layout

```
sdmc:/3ds/ff9/voice/pack.txt              (any content: marks the pack as present; checked once)
sdmc:/3ds/ff9/voice/<bank>/<message>.opus (one file per voiced line)
```

- **message**: the line's number in its text bank, the u16 the event script passes to its message opcodes
  (`MsgWindow.msgId`).
- **bank**: which text bank holds the line:

| where | bank |
|---|---|
| a field | the field's id (`GameData.fieldId`: the dark room at the start is 50, Dali's inn 351) |
| a battle | 10000 + the battle's id (`GameData.battleId`) |
| the world map | 20000 |
| anything else | 30000 + the game's mode |

Lines with no file play nothing (and stop the line before them). A file is Ogg Opus, mono or stereo, at any bitrate
(32-48 kbit/s mono suits speech); it plays as 48 kHz stereo on its own DSP channel, with the music lowered under it,
until it ends or its window closes (the player's confirm, the script, a scene change).

## Finding a line's id

Build with `VOICELOG=1` and play to the line: the log (`sdmc:/3ds/ff9/log.txt`) gets one entry per line shown,

```
voice: line 50/34 (window 2)          (bank 50, message 34: "Sure is dark...")
```

and, for each recording, when it ended or was cut off. `tests/scenarios` fixtures (`SCENARIO=`) reach a field
directly.

## How it works
`port/game/voice_hook.c` wraps `msg_open` (`config/renames.txt`), which every dialogue window opens through, and
watches the voiced window each frame to stop the recording when it closes or shows another line.
`port/3ds/voice.c` decodes with libopusfile on a thread on the second core (beside the SPU), four 85 ms buffers ahead of
the DSP.
