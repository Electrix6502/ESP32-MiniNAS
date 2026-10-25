"""Linux integration test of the actual SMB adapter. Requires GCC/G++ and impacket.
python -m pip install -r tests/requirements.txt
python scripts/test_native.py --sanitize
"""
from pathlib import Path
import argparse
import concurrent.futures
import io
import json
import socket
import struct
import subprocess
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]

def build(destination,sanitizer=False):
    lib=ROOT/'lib/libsmb2'
    destination.mkdir(parents=True,exist_ok=True)
    (destination/'config.h').write_text((lib/'include/esp/config.h').read_text())
    includes=[destination,lib/'include',lib/'include/smb2',lib/'lib',ROOT/'include']
    flags=['-g','-O0' if sanitizer else '-O1','-DHAVE_CONFIG_H','-D_U_=__attribute__((unused))']
    if sanitizer:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    flags += [f'-I{p}' for p in includes]
    manifest=json.loads((lib/'library.json').read_text())
    sources=list(dict.fromkeys(lib/'lib'/f[2:-1] for f in manifest['build']['srcFilter'] if f.startswith('+<')))
    def compile_file(source):
        obj=destination/(source.stem+'.o')
        subprocess.run(['gcc','-std=gnu11',*flags,'-c',str(source),'-o',str(obj)],check=True)
        return obj
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:objects=list(pool.map(compile_file,sources))
    for source in [ROOT/'src/SmbServer.cpp',ROOT/'src/Path.cpp',ROOT/'tests/native/HostStorage.cpp']:
        obj=destination/(source.stem+'-cpp.o')
        subprocess.run(['g++','-std=c++17',*flags,'-c',str(source),'-o',str(obj)],check=True)
        objects.append(obj)
    binary=destination/'mininas-test-server'
    subprocess.run(['g++',*flags,*map(str,objects),'-lpthread','-o',str(binary)],check=True)
    return binary

def run_tests(binary):
    from impacket.smbconnection import SMBConnection,SessionError
    from impacket.smb3 import SMB3
    results=[]
    with tempfile.TemporaryDirectory(prefix='mininas-smb-') as directory:
        root=Path(directory);flash=root/'flash';sd=root/'sd';flash.mkdir();sd.mkdir()
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
        log=root/'server.log'
        with log.open('w+') as output:
            process=subprocess.Popen([str(binary),str(flash),str(sd),str(port)],stdout=output,stderr=output)
            try:
                for _ in range(100):
                    if process.poll() is not None:raise RuntimeError(log.read_text())
                    try:
                        with socket.create_connection(('127.0.0.1',port),timeout=.1):break
                    except OSError:time.sleep(.05)
                time.sleep(.15)
                for dialect in [0x0202,0x0210,0x0300,0x0302,0x0311]:
                    print(f'Testing SMB {dialect:04x}',flush=True)
                    # The SMB3 engine supports 3.0.2, absent from the facade's dialect list.
                    conn=SMBConnection(existingConnection=SMB3('espnas','127.0.0.1',sess_port=port,preferredDialect=dialect,timeout=6))
                    conn.login('','');assert conn.isGuestSession()
                    name=f'test-{dialect:04x}';conn.createDirectory('flash',name)
                    payload=bytes(range(256))*4096+b'MiniNAS-test-end'
                    conn.putFile('flash',name+'/binary.dat',io.BytesIO(payload).read)
                    received=io.BytesIO();conn.getFile('flash',name+'/binary.dat',received.write)
                    assert received.getvalue()==payload
                    conn.rename('flash',name+'/binary.dat',name+'/renamed.dat')
                    assert 'renamed.dat' in [x.get_longname() for x in conn.listPath('flash',name+'/*')]
                    assert (flash/name/'renamed.dat').read_bytes()==payload
                    conn.putFile('sd','sd-only.txt',io.BytesIO(b'SD volume').read)
                    assert (sd/'sd-only.txt').read_bytes()==b'SD volume'
                    assert not (flash/'sd-only.txt').exists()
                    conn.deleteFile('sd','sd-only.txt')
                    try:
                        conn.getFile('flash','../outside.txt',lambda _:None)
                        raise AssertionError('Traversal accepted')
                    except SessionError:pass
                    conn.deleteFile('flash',name+'/renamed.dat');conn.deleteDirectory('flash',name)
                    assert not (flash/name).exists()
                    conn.logoff();conn.close();time.sleep(.15)
                    results.append(f'PASS SMB {dialect:04x}: guest, mkdir, 1 MiB binary read/write, rename, list, delete, two shares, traversal rejection, repeated logoff')
                for i in range(160):(flash/f'Datei-{i:03d}-Grüße-'.ljust(100,'x')).write_bytes(b'')
                conn=SMBConnection('espnas','127.0.0.1',sess_port=port,preferredDialect=0x0311,timeout=6)
                conn.login('arbitrary-user','arbitrary-password');assert conn.isGuestSession()
                assert len(conn.listPath('flash','*'))==160
                results.append('PASS SMB 0311: arbitrary credentials map to guest; 160 long Unicode names across directory pages')
                other=SMBConnection('espnas','127.0.0.1',sess_port=port,preferredDialect=0x0311,timeout=6);other.login('','')
                tid=conn.connectTree('flash');tid2=other.connectTree('flash')
                fid=conn.openFile(tid,'locked.dat',desiredAccess=0x001F01FF,shareMode=1,creationOption=0x40,creationDisposition=3)
                try:
                    other.openFile(tid2,'locked.dat',desiredAccess=0x001F01FF,shareMode=7,creationOption=0x40,creationDisposition=1)
                    raise AssertionError('Conflicting writer accepted')
                except SessionError as ex:assert ex.getErrorCode()==0xC0000043
                conn.getSMBServer().setInfo(tid,fid,struct.pack('<Q',4096),infoType=1,fileInfoClass=20)
                assert (flash/'locked.dat').stat().st_size==4096
                conn.closeFile(tid,fid);conn.deleteFile('flash','locked.dat');other.close();conn.logoff();conn.close()
                results.append('PASS SMB 0311: two clients, conflicting writer rejected, SET_INFO EOF size change')
            except Exception:
                process.terminate();process.wait(timeout=10)
                print(f'SERVER EXIT {process.returncode}\n'+log.read_text());raise
            finally:
                if process.poll() is None:process.terminate();process.wait(timeout=10)
            errors=log.read_text()
            if process.returncode!=0:raise RuntimeError(f'Server exit {process.returncode}\n{errors}')
            if any(x in errors for x in ['runtime error:','ERROR: AddressSanitizer','ERROR: UndefinedBehaviorSanitizer']):
                raise RuntimeError('Sanitizer error:\n'+errors)
    for result in results:print(result)
    return results

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--sanitize',action='store_true');parser.add_argument('--build-only',action='store_true');args=parser.parse_args()
    binary=build(ROOT/'.pio/native-tests',args.sanitize)
    if not args.build_only:run_tests(binary)
