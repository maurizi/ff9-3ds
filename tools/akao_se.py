"""One of the game's sound effects as PCM, from the disc's own data: its sequence run as the game's sound driver runs it
(decomp/src/main/akao*.c: the sound-effect channels' part, and only the opcodes the banner's sound uses: any other
raises), the voices mixed as the PS1's sound chip mixes them (port/psx/spu_*.c: ADPCM, the Gaussian interpolation,
the envelope, a voice's pitch modulated by the one before, the reverb at its own 22.05 kHz). Plain Python: the
builder runs it (tools/banner3d.py), in a browser too.

Checked against the PC build playing the same sound effect alone (a scenario's `se=` key, --spu-trace and --audio):
the register writes are the same values at the same ticks.
"""
import struct

RATE = 44100
TICK = RATE / 240                # the driver's timer: 240 a second
SE_TEMPO = 0x66A8                # g_akaoSeState.tempo's high half: a sound effect steps at 240 * this / 65536 a second
NOTE_LENGTHS = (192, 96, 48, 24, 12, 6, 3, 32, 16, 8, 4)  # (g_akaoNoteLengths: ticks by note opcode % 11)
NOTE_TIE, NOTE_REST = 0x84, 0x8F
VD_VOL, VD_PITCH = 1, 2
LEGATO, LEGATO_TIED = 1, 2
# The SPU's reverb registers 0x1C0-0x1FE for the SDK's reverb mode 4 (Studio C, libspu's table: what the driver's
# akao_setReverbMode(4) leaves there), and the work area's start (register 0x1A2)
REVERB_MODES = {4: (0xF204, (
    0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C,
    0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238,
    0x0154, 0x00AA, 0x8000, 0x8000))}
MAIN_VOLUME = 0x3FFF             # (registers 0x180 / 0x182, as the driver sets them)
# The SPU's Gaussian interpolation table (hardware: psx-spx, "4-point Gaussian interpolation"; port/include/
# psx_spu_gauss.h)
GAUSS = [
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33,
    35, 37, 39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80, 84, 87, 91, 95, 99, 103, 107, 111, 116,
    120, 125, 130, 135, 140, 145, 150, 156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250,
    257, 266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412, 424, 436, 448, 460, 473,
    485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627, 643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816,
    835, 854, 874, 894, 914, 935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260,
    1286, 1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756, 1789,
    1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334, 2374, 2415, 2456,
    2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032, 3079, 3128, 3176, 3225, 3275,
    3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855, 3910, 3967, 4023, 4081, 4138, 4197, 4255,
    4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807, 4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399,
    5468, 5536, 5606, 5676, 5746, 5817, 5888, 5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702,
    6779, 6856, 6934, 7012, 7091, 7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150,
    8234, 8319, 8404, 8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720,
    9811, 9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194,
    11288, 11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714,
    12809, 12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248,
    14343, 14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762,
    15855, 15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220,
    17308, 17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584,
    18665, 18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816,
    19888, 19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884,
    20944, 21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754,
    21801, 21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402,
    22435, 22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809,
    22826, 22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962,
    22963,
]


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s8(v):
    return v - 256 if v & 0x80 else v


def cdiv(a, b):
    """C's integer division (towards zero)"""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def clamp16(v):
    return 32767 if v > 32767 else -32768 if v < -32768 else v


# ---- the bank: instruments and samples -----------------------------------------------------------------------------

class Bank:
    """An AKAO instrument bank (AkaoBankHeader: SPU address, sample bytes, first instrument slot, count; 16-byte
    instruments {start, loop, fine tune, base note, ADSR1, ADSR2}, addresses from the samples' start; the samples)"""

    def __init__(self, data):
        if data[:4] != b'AKAO':
            raise ValueError('sound: not an AKAO bank')
        self.id, = struct.unpack_from('<i', data, 4)
        self.spu, size, self.slot, count = struct.unpack_from('<4i', data, 0x10)
        self.instruments = [struct.unpack_from('<iihhHH', data, 0x40 + 16 * i) for i in range(count)]
        self.samples = bytes(data[0x40 + 16 * count:0x40 + 16 * count + size])


def calc_pitch(instr, note, semitones):
    """akao_calcPitch (without a channel's fine tune): the SPU pitch of a note on an instrument"""
    fine, base = instr[2], instr[3]
    semi = semitones[(note - base) % 12]
    if fine == 0:
        freq = semi << 8
    elif fine < 0:
        freq = semi * (fine & 0xFFFF) >> 8
    else:
        freq = (semi * fine >> 7) + (semi << 8)
    if note < base:
        n = note
        while True:
            freq >>= 1
            n += 12
            if n >= base:
                break
    else:
        freq <<= (note - base) // 12
    return freq >> 8


# ---- the sequence: what the driver writes to the SPU, tick by tick ------------------------------------------------

OPERANDS = {0xA1: 1, 0xA3: 1, 0xA4: 2, 0xA5: 1, 0xA8: 1, 0xA9: 2, 0xC2: 0, 0xC6: 0, 0xDA: 1}  # (the ones handled)


class Channel:
    def __init__(self, data, pc, second):
        self.data, self.pc, self.second = data, pc, second
        self.expression, self.volume = 0x6E00, 0x23000000   # (akao_resetChannel)
        self.se_volume = 0x7F00                             # (akao_startSeChannel: volume 0x7F)
        self.note_ticks, self.gate_ticks = 2, 1
        self.octave = self.legato = self.portamento = self.prev_key = self.cur_key = 0
        self.slide_len = self.slide_keys = self.pitch = self.pitch_slide = self.pitch_slide_left = 0
        self.pitch_slide_step = self.volume_slide_len = self.volume_step = 0
        self.dirty = 0
        self.instr = None
        self.active = True
        self.reverb = self.pitch_mod = self.playing = self.key_on = self.key_off = False

    def byte(self):
        v = self.data[self.pc]
        self.pc += 1
        return v

    def next_note(self):
        """akao_peekNextNote for the sequences this module takes: the next note, tie or rest opcode (0xA0: none
        before the end); a rest or the end also ends a legato"""
        p = self.pc
        while True:
            op = self.data[p]
            if op < 0x9A:
                break
            if op in OPERANDS:
                p += 1 + OPERANDS[op]
            elif op == 0xFE and self.data[p + 1] in (0x1A, 0x1B):
                p += 2
            else:
                op = 0xA0
                break
        if op >= NOTE_REST:
            self.legato &= ~LEGATO
        return op


def _exec(ch, bank, semitones):
    """akao_execChannel: opcodes up to the next note, tie or rest (False: the sequence ended)"""
    while True:
        op = ch.byte()
        if op < 0xA0:
            break
        if op == 0xA0 or op == 0xFF:
            return False
        if op == 0xA1:                       # program
            program = ch.byte()
            if not bank.slot <= program < bank.slot + len(bank.instruments):
                raise ValueError(f'sound: instrument {program:#x} is not in the bank')
            ch.instr = bank.instruments[program - bank.slot]
        elif op == 0xA3:
            ch.expression = ch.byte() << 8
            ch.dirty |= VD_VOL
        elif op == 0xA4:
            ch.slide_len = ch.byte() or 0x100
            ch.slide_keys = s8(ch.byte())
        elif op == 0xA5:
            ch.octave = ch.byte()
        elif op == 0xA8:
            ch.volume = s8(ch.byte()) << 23
            ch.volume_slide_len = 0
            ch.dirty |= VD_VOL
        elif op == 0xA9:
            ch.volume_slide_len = ch.byte() or 0x100
            cur = ch.volume & ~0xFFFF
            ch.volume_step = cdiv((s8(ch.byte()) << 23) - cur, ch.volume_slide_len)
            ch.volume = cur
        elif op == 0xC2:
            ch.reverb = True
        elif op == 0xC6:                     # (only a two-channel sound's second channel is modulated)
            ch.pitch_mod = ch.second
        elif op == 0xDA:
            ch.portamento = ch.byte() or 0x100
            ch.prev_key = 0
            ch.legato = LEGATO
        elif op == 0xFE and ch.data[ch.pc] in (0x1A, 0x1B):   # (music's full gate: nothing for a sound effect)
            ch.pc += 1
        else:
            raise ValueError(f'sound: sequence opcode {op:#x} is not handled')
    nxt = ch.next_note()
    ch.note_ticks = ch.gate_ticks = NOTE_LENGTHS[op % 11]
    if (nxt < NOTE_TIE or nxt >= NOTE_REST) and not ch.legato & LEGATO:
        ch.gate_ticks -= 2
    if op >= NOTE_REST:
        ch.portamento = 0
        ch.legato &= ~LEGATO_TIED
        return True
    if op < NOTE_TIE:
        key = op // 11 + ch.octave * 12
        if not ch.legato & LEGATO_TIED:
            ch.key_on = True
            ch.pitch_slide_left = 0
        if ch.portamento and ch.prev_key:
            ch.slide_len = ch.portamento
            ch.slide_keys = key - ch.prev_key
            ch.cur_key = ch.prev_key
            key = ch.prev_key
        else:
            ch.cur_key = key
        if ch.instr is None:
            raise ValueError('sound: a note before a program')
        ch.pitch = calc_pitch(ch.instr, key, semitones)
        ch.playing = True
        ch.dirty |= VD_VOL | VD_PITCH
        ch.pitch_slide = 0
    ch.legato = (ch.legato & ~LEGATO_TIED) | ((ch.legato & LEGATO) << 1)
    if ch.slide_keys:
        ch.cur_key += ch.slide_keys
        target = calc_pitch(ch.instr, ch.cur_key, semitones) << 16
        ch.pitch_slide_left = ch.slide_len
        ch.slide_keys = 0
        ch.pitch_slide_step = cdiv(target - ((ch.pitch << 16) + ch.pitch_slide), ch.pitch_slide_left)
    ch.prev_key = ch.cur_key
    return True


def events(se, bank, semitones, pan_centre):
    """[(sample, voice, what, value)] in time order: what 'on' (value: the instrument), 'off', 'pitch', 'vol',
    'mods' ((reverb, pitch modulated)): the SPU writes of a sound effect item ("AKAO", track count, bank id, its
    tracks' offsets) played alone at full volume, centred (akao_cmdPlaySeBank, akao_tick)"""
    if se[:4] != b'AKAO' or struct.unpack_from('<I', se, 4)[0] != 1:
        raise ValueError('sound: not a one-entry sound effect')
    at = 0x14 + struct.unpack_from('<I', se, 0x10)[0]
    o0, o1 = struct.unpack_from('<2H', se, at)
    tracks = [None if o0 == 0xFFFF else at + 4 + o0, None if o1 == 0xFFFF else at + 4 + o1]
    chans = [Channel(se, pc, i == 1 and tracks[0] is not None) for i, pc in enumerate(tracks) if pc is not None]
    out, acc, tick = [], 0, 0
    while any(c.active or c.key_off for c in chans):
        t = int(tick * TICK)
        for v, c in enumerate(chans):        # akao_flushKeyOffs
            if c.key_off:
                c.key_off = False
                out.append((t, v, 'off', None))
        for v, c in enumerate(chans):        # akao_updateVoices: akao_calcSeChannelVoice, the dirty registers
            if not (c.active and c.playing):
                continue
            if c.dirty & VD_VOL:
                vol = ((c.volume >> 16) * (c.expression >> 8)) >> 7
                vol = (vol * (c.se_volume >> 8)) >> 7
                out.append((t, v, 'vol', ((vol * pan_centre) >> 15) & 0x7FFF))
            if c.dirty & VD_PITCH:
                out.append((t, v, 'pitch', (c.pitch + (c.pitch_slide >> 16)) & 0x3FFF))
            c.dirty = 0
            if c.key_on:
                c.key_on = False
                out.append((t, v, 'mods', (c.reverb, c.pitch_mod)))
                out.append((t, v, 'on', c.instr))
        acc += SE_TEMPO
        if acc & ~0xFFFF:
            acc &= 0xFFFF
            for c in chans:
                if not c.active:
                    continue
                c.note_ticks -= 1
                c.gate_ticks -= 1
                if c.note_ticks == 0:
                    if not _exec(c, bank, semitones):
                        c.active = c.playing = False
                        c.key_off = True
                        continue
                elif c.gate_ticks == 0:
                    c.key_off = True
                    c.playing = False
                if c.volume_slide_len:       # akao_stepChannelSlides
                    c.volume_slide_len -= 1
                    new = c.volume + c.volume_step
                    if (new ^ c.volume) & 0xFFE00000:
                        c.dirty |= VD_VOL
                    c.volume = new
                if c.pitch_slide_left:
                    c.pitch_slide_left -= 1
                    new = c.pitch_slide + c.pitch_slide_step
                    if (new ^ c.pitch_slide) & ~0xFFFF:
                        c.dirty |= VD_PITCH
                    c.pitch_slide = new
        tick += 1
        if tick > 240 * 20:
            raise ValueError('sound: the sequence does not end')
    return out


# ---- the SPU ---------------------------------------------------------------------------------------------------------

class Envelope:
    """port/psx/spu_envelope.c's ADSR"""
    ATTACK, DECAY, SUSTAIN, RELEASE, OFF = range(5)

    def __init__(self):
        self.level, self.adsr1, self.adsr2 = 0, 0, 0
        self._phase(self.OFF)

    def _phase(self, phase):
        rate, mask, decrease, exponential, self.target = 0, 0x7F, False, False, 0
        self.phase = phase
        if phase == self.ATTACK:
            rate, exponential, self.target = self.adsr1 >> 8 & 0x7F, bool(self.adsr1 >> 15), 32767
        elif phase == self.DECAY:
            rate, mask, decrease, exponential = (self.adsr1 >> 4 & 15) * 4, 0x7C, True, True
            self.target = min(32767, ((self.adsr1 & 15) + 1) * 0x800)
        elif phase == self.SUSTAIN:
            rate, decrease, exponential = self.adsr2 >> 6 & 0x7F, bool(self.adsr2 >> 14 & 1), bool(self.adsr2 >> 15)
        elif phase == self.RELEASE:
            rate, mask, decrease, exponential = (self.adsr2 & 31) * 4, 0x7C, True, bool(self.adsr2 >> 5 & 1)
        shift, step = rate >> 2, 7 - (rate & 3)
        if decrease:
            step = -step - 1
        if shift < 11:
            step *= 1 << (11 - shift)
        self.rate, self.decrease, self.exponential, self.step, self.counter = rate, decrease, exponential, step, 0
        self.increment = 0x8000 >> (shift - 11 if shift > 11 else 0)
        if not self.increment and rate & mask != mask:
            self.increment = 1

    def key_on(self, adsr1, adsr2):
        self.adsr1, self.adsr2, self.level = adsr1, adsr2, 0
        self._phase(self.ATTACK)

    def key_off(self):
        if self.phase not in (self.OFF, self.RELEASE):
            self._phase(self.RELEASE)

    def off(self):
        self.level = 0
        self._phase(self.OFF)

    def tick(self):
        if self.phase == self.OFF:
            return
        if self.increment:
            increment, step = self.increment, self.step
            if self.exponential:
                if self.decrease:
                    step = (step * self.level) >> 15
                elif self.level >= 0x6000:
                    if self.rate < 40:
                        step >>= 2
                    elif self.rate >= 44:
                        increment >>= 2
                    else:
                        step >>= 1
                        increment >>= 1
            self.counter += increment
            if self.counter & 0x8000:
                self.counter = 0
                self.level = max(0, self.level + step) if self.decrease else clamp16(self.level + step)
        if self.phase == self.SUSTAIN:
            return
        if (self.level <= self.target) if self.decrease else (self.level >= self.target):
            self._phase(self.OFF if self.phase == self.RELEASE else self.phase + 1)


class Voice:
    COEFF = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))

    def __init__(self, samples):
        self.ram, self.env = samples, Envelope()
        self.pitch = self.vol = 0
        self.reverb = self.pitch_mod = False
        self.buf, self.counter, self.addr, self.loop, self.hist, self.flags = [0] * 31, 0, 0, 0, (0, 0), 0

    def _block(self):
        b = self.ram[self.addr:self.addr + 16]
        if len(b) < 16:
            b = bytes(16)
        shift, filt = b[0] & 15, b[0] >> 4
        if shift > 12:
            shift = 9
        c1, c2 = self.COEFF[filt] if filt < 5 else (0, 0)
        prev, older = self.hist
        out = self.buf[28:]                  # (the last three, for the interpolation)
        for i in range(28):
            n = b[2 + i // 2] >> (4 * (i & 1)) & 15
            s = ((n - 16 if n & 8 else n) << 12) >> shift
            s += (prev * c1) >> 6
            s += (older * c2) >> 6
            older, prev = prev, clamp16(s)
            out.append(prev)
        self.buf, self.hist, self.flags = out, (prev, older), b[1]

    def key_on(self, instr):
        self.addr, self.loop = instr[0], instr[1]
        self.counter, self.hist, self.buf = 0, (0, 0), [0] * 31
        self._block()
        self.env.key_on(instr[4], instr[5])

    def sample(self, previous):
        """this voice's sample after its envelope; previous: the voice before's (its pitch's modulation)"""
        env = self.env
        if env.phase == Envelope.OFF:
            return 0
        out = 0
        if env.level:
            at, g = self.counter >> 12, self.counter >> 4 & 255
            b = self.buf
            out = (b[at] * GAUSS[255 - g] + b[at + 1] * GAUSS[511 - g] + b[at + 2] * GAUSS[256 + g]
                   + b[at + 3] * GAUSS[g]) >> 15
            out = (out * env.level) >> 15
        pitch = self.pitch
        if self.pitch_mod:
            pitch = (pitch * (clamp16(previous) + 32768) >> 15) & 0xFFFF
        env.tick()
        self.counter += min(pitch, 0x3FFF)
        if self.counter >= 28 << 12:
            self.counter -= 28 << 12
            self.addr += 16
            if self.flags & 1:
                self.addr = self.loop
                if not self.flags & 2:
                    env.off()
            self._block()
        return out


class Reverb:
    """port/psx/spu_reverb.c's psx_spu_reverb_tick_half: the SPU's reverb at its own rate, 22.05 kHz"""

    def __init__(self, base, regs):
        self.base = self.cursor = base * 4
        self.r = [s16(v) if 2 <= i < 10 or i >= 30 else v for i, v in enumerate(regs)]
        self.ram = {}

    def _at(self, reg, delta=0):
        s = self.cursor + ((reg * 4 + delta) & 0x3FFFF)
        if s & 0x40000:
            s += self.base
        return s & 0x3FFFF

    def _step(self, ch, x):
        r, ram, at = self.r, self.ram, self._at
        d1, d2, iir, c1, c2, c3, c4, wall, apf1, apf2 = r[:10]
        incoming = (x * r[30 + ch]) >> 14
        same = (ram.get(at(r[16 + ch]), 0) * wall) >> 14
        cross = (ram.get(at(r[24 + (ch ^ 1)]), 0) * wall) >> 14
        filtered = []
        for refl, dest in ((clamp16((same + incoming) >> 1), 10 + ch), (clamp16((cross + incoming) >> 1), 18 + ch)):
            old = ram.get(at(r[dest], -1), 0)
            fresh = (refl * iir) >> 14
            kept = (0 if old == -32768 else old * -65536) if iir == -32768 else old * (32768 - iir)
            filtered.append(clamp16((fresh + (kept >> 14)) >> 1))
        ram[at(r[10 + ch])] = filtered[0]
        ram[at(r[18 + ch])] = filtered[1]
        comb = sum((ram.get(at(r[a + ch]), 0) * c) >> 14 for a, c in ((12, c1), (14, c2), (20, c3), (22, c4)))
        del1 = ram.get(at((r[26 + ch] - d1) & 0xFFFF), 0)
        del2 = ram.get(at((r[28 + ch] - d2) & 0xFFFF), 0)
        st1 = clamp16((comb + ((del1 * (32767 if apf1 == -32768 else -apf1)) >> 14)) >> 1)
        st2 = clamp16(del1 + ((((st1 * apf1) >> 14) + ((del2 * (32767 if apf2 == -32768 else -apf2)) >> 14)) >> 1))
        wet = clamp16(del2 + ((st2 * apf2) >> 15))
        ram[at(r[26 + ch])] = st1
        ram[at(r[28 + ch])] = st2
        return wet

    def tick(self, left, right):
        wr = self._step(1, right)
        self.cursor = (self.cursor + 1) & 0x3FFFF or self.base
        return self._step(0, left), wr


def render(se, bank, semitones, pan_centre, reverb=None, tail=1.2, limit=3.0):
    """(rate, [(left, right)]): the sound effect `se` on the instruments of `bank` (Bank), as the SPU plays the
    driver's writes; reverb: (mode, depth) as the music playing has them (its sequence header's mode, its depth
    opcode), None for none; tail: seconds of reverb kept after the last voice stops; limit: seconds at most"""
    evs = events(se, bank, semitones, pan_centre)
    voices = [Voice(bank.samples) for _ in range(1 + max(e[1] for e in evs))]
    rv, depth = None, 0
    if reverb:
        if reverb[0] not in REVERB_MODES:
            raise ValueError(f'sound: reverb mode {reverb[0]} is not handled')
        rv, depth = Reverb(*REVERB_MODES[reverb[0]]), reverb[1]
    out, e, wet, send, quiet = [], 0, (0, 0), [0, 0], 0
    main = MAIN_VOLUME * 2
    for t in range(int(limit * RATE)):
        while e < len(evs) and evs[e][0] <= t:
            _, v, what, val = evs[e]
            e += 1
            vc = voices[v]
            if what == 'on':
                vc.key_on(val)
            elif what == 'off':
                vc.env.key_off()
            elif what == 'pitch':
                vc.pitch = val
            elif what == 'vol':
                vc.vol = s16(val << 1)       # (a register's 15 bits, doubled: the SPU's volume level)
            else:
                vc.reverb, vc.pitch_mod = val
        dry, wet_in, previous = 0, 0, 0
        for vc in voices:
            previous = vc.sample(previous)
            x = (previous * vc.vol) >> 15
            dry += x
            if vc.reverb:
                wet_in += x
        if rv:                               # (centred voices: left and right get the same)
            send[t & 1] = clamp16(wet_in)
            if t & 1:
                wet = rv.tick((send[0] + send[1]) >> 1, (send[0] + send[1]) >> 1)
        left = clamp16((clamp16(dry + ((wet[0] * depth) >> 15)) * main) >> 15)
        right = clamp16((clamp16(dry + ((wet[1] * depth) >> 15)) * main) >> 15)
        out.append((left, right))
        live = e < len(evs) or any(vc.env.phase != Envelope.OFF for vc in voices)
        quiet = 0 if live else quiet + 1
        if quiet > tail * RATE:
            break
    return RATE, out
