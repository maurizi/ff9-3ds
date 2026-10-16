"""GDB transport for synthetic GPU probes in a dedicated DuckStation instance."""
import socket
import struct

def words(values):
    return struct.pack('<'+'I'*len(values),*values)

def xy(x,y):
    return x|(y<<16)

class DuckStation:
    def __init__(self,port):
        self.socket=socket.create_connection(('127.0.0.1',port),timeout=10)
        self.socket.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        self.command('?')
        self.registers=self.command('g')
        self.saved=[(address,self.read(address,size)) for address,size in
                    [(0x801f8000,0x6e00),(0x801fee00,128),(0x801f0000,32)]]
    def reply(self):
        def byte():
            value=self.socket.recv(1)
            if not value:raise RuntimeError('DuckStation GDB disconnected')
            return value
        while byte()!=b'$':pass
        data=bytearray()
        while True:
            c=byte()
            if c==b'#':break
            data.extend(c)
        checksum=byte()+byte()
        if int(checksum,16)!=(sum(data)&255):raise RuntimeError('bad GDB checksum')
        self.socket.sendall(b'+')
        return data.decode()
    def command(self,text):
        data=text.encode();self.socket.sendall(b'$'+data+b'#'+f'{sum(data)&255:02x}'.encode())
        return self.reply()
    def read(self,address,size):
        return b''.join(bytes.fromhex(self.command(f'm{address+i:x},{min(512,size-i):x}')) for i in range(0,size,512))
    def write(self,address,data):
        for i in range(0,len(data),512):
            part=data[i:i+512]
            if self.command(f'M{address+i:x},{len(part):x}:'+part.hex())!='OK':raise RuntimeError('GDB write failed')
    def draw(self,packet):
        packet=packet+[0xc0000000,xy(4,4),xy(4,4)]
        payload=words(packet)
        if len(payload)>0x6e00:raise ValueError('synthetic GPU packet overlaps probe code')
        # Wait for GPUSTAT's ready-to-read bit before consuming GPUREAD.
        code=[0x3c091f80,0x35291810,0xad200004,0x8c880000,0x24840004,0xad280000,0x1485fffc,0,
              0x8d280004,0x3c0a0800,0x010a4024,0x1100fffc,0,
              0x8d280000,0x24c60004,0xacc8fffc,0x14c7fffc,0,0]
        self.write(0x801f8000,payload);self.write(0x801fee00,words(code))
        registers=[self.registers[i:i+8] for i in range(0,len(self.registers),8)]
        for index,value in [(4,0x801f8000),(5,0x801f8000+len(payload)),(6,0x801f0000),
                            (7,0x801f0020),(32,0),(37,0x801fee00)]:
            registers[index]=struct.pack('<I',value).hex()
        if self.command('G'+''.join(registers))!='OK':raise RuntimeError('GDB register write failed')
        if self.command('Z0,801fee48,4')!='OK':raise RuntimeError('GDB breakpoint failed')
        try:
            if not self.command('c').startswith(('S05','T05')):raise RuntimeError('GPU probe did not reach breakpoint')
            if self.command('g')[37*8:38*8]!='48ee1f80':raise RuntimeError('GPU probe stopped at wrong PC')
            return self.read(0x801f0000,32)
        finally:self.command('z0,801fee48,4')
    def close(self):
        try:
            for address,data in self.saved:self.write(address,data)
            registers=self.registers
            if getattr(self,'destructive',False):
                # The test has overwritten game RAM. Park the CPU instead of
                # resuming that invalid game when the GDB socket disconnects.
                self.write(0x801fed00,words([0x0807fb40,0]))
                r=[registers[i:i+8] for i in range(0,len(registers),8)]
                r[32]='00000000';r[37]=struct.pack('<I',0x801fed00).hex()
                registers=''.join(r)
            self.command('G'+registers)
        finally:self.socket.close()

    def execute(self,code,start_address,end_address):
        """Run probe code with a0/a1 delimiting a buffer. Overwrites test RAM."""
        self.destructive=True
        self.write(0x801fee00,words(code+[0]))
        registers=[self.registers[i:i+8] for i in range(0,len(self.registers),8)]
        for index,value in [(4,start_address),(5,end_address),(32,0),(37,0x801fee00)]:
            registers[index]=struct.pack('<I',value).hex()
        if self.command('G'+''.join(registers))!='OK':raise RuntimeError('GDB register write failed')
        end=0x801fee00+len(code)*4
        if self.command(f'Z0,{end:x},4')!='OK':raise RuntimeError('GDB breakpoint failed')
        try:
            if not self.command('c').startswith(('S05','T05')):raise RuntimeError('GPU replay stopped unexpectedly')
            if self.command('g')[37*8:38*8]!=struct.pack('<I',end).hex():
                raise RuntimeError('GPU replay stopped at wrong PC')
        finally:self.command(f'z0,{end:x},4')

    def replay_writes(self,writes):
        """Destructive dedicated-instance replay; writes (GP0/GP1 port, value)."""
        for start in range(0,len(writes),32768):
            payload=b''.join(struct.pack('<II',0x1f801810+port*4,value)
                             for port,value in writes[start:start+32768])
            self.write(0x80010000,payload)
            # Honour GPU FIFO backpressure; a tight CPU loop can otherwise
            # outrun polygon processing and lose later command words.
            self.execute([0x3c0a1f80,0x354a1814,0x3c0c1000,
                          0x8d4b0000,0,0x016c5824,0x1160fffc,0,
                          0x8c880000,0x8c890004,0x24840008,0xad090000,0x1485fff6,0],
                         0x80010000,0x80010000+len(payload))

    def read_vram(self):
        """Full 1024x512 VRAM, waiting for the GPU read-transfer ready bit."""
        self.execute([0x3c081f80,0x35081810,0x3c090400,0xad090004,
                      0x3c09c000,0xad090000,0xad000000,
                      0x3c090200,0x35290400,0xad090000,0x8d090004,0x3c0a0800,
                      0x012a4824,0x1120fffc,0,0x8d090000,0x24840004,0xac89fffc,
                      0x1485fffc,0],0x80010000,0x80110000)
        return self.read(0x80010000,1024*512*2)
