"""Keep organization HTTP/LSP features working when merging preview changes."""
import argparse
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import time
import unittest
import sys

p=argparse.ArgumentParser()
p.add_argument('--forge',required=True)
p.add_argument('--root',required=True)
p.add_argument('--lib-dir',required=True)
p.add_argument('--cc',required=True)
p.add_argument('--lsp',required=True)
config,args=p.parse_known_args();sys.argv=[sys.argv[0],*args]

class OrganizationIntegration(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.folder=Path(self.temp.name)
    def compile(self,source,name='app'):
        path=self.folder/(name+'.fg');binary=self.folder/name;path.write_text(source)
        r=subprocess.run([config.forge,str(path),'-o',str(binary),'--forge-root',config.root,'--lib-dir',config.lib_dir,'--cc',config.cc],capture_output=True,text=True,timeout=30)
        self.assertEqual(r.returncode,0,r.stderr);return binary
    def port(self):
        with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
    def request(self,process,port,path='/api/health',tls=False):
        deadline=time.monotonic()+5
        while True:
            try:
                sock=socket.create_connection(('127.0.0.1',port),timeout=.5);break
            except OSError:
                if process.poll() is not None:self.fail('HTTP server exited before listening')
                if time.monotonic()>deadline:self.fail('HTTP listener timeout')
                time.sleep(.02)
        if tls:sock=ssl._create_unverified_context().wrap_socket(sock,server_hostname='localhost')
        with sock:
            sock.settimeout(3);sock.sendall(('GET '+path+' HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n').encode())
            data=b''
            while b'\r\n\r\n' not in data:data+=sock.recv(65536)
            headers,body=data.split(b'\r\n\r\n',1)
            lengths=[x.split(b':',1)[1].strip() for x in headers.split(b'\r\n') if x.lower().startswith(b'content-length:')]
            expected=int(lengths[0]) if lengths else len(body)
            while len(body)<expected:body+=sock.recv(65536)
        self.assertIn(b'200',headers.split(b'\r\n')[0]);return body
    def server(self,binary):
        process=subprocess.Popen([str(binary)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        def stop():
            if process.poll() is None:process.terminate()
            try:process.wait(timeout=2)
            except subprocess.TimeoutExpired:process.kill();process.wait(timeout=2)
        self.addCleanup(stop);return process
    def test_http_serving_modes(self):
        for mode in ['prepared','mt','hybrid','uring','sendfile']:
            with self.subTest(mode=mode):
                port=self.port()
                prepare='http_prepare_sendfile' if mode=='sendfile' else 'http_prepare'
                serve={'prepared':'http_serve_prepared(s);','sendfile':'http_serve_prepared(s);','mt':'http_serve_mt(s,2);','hybrid':'http_serve_hybrid(s,2);','uring':'http_serve_uring(s,2);'}[mode]
                binary=self.compile('import http;native main{let s: int=http_listen('+str(port)+');'+prepare+'(s,"mode '+mode+'");'+serve+'}',mode)
                process=self.server(binary);self.assertEqual(self.request(process,port),('mode '+mode).encode())
    def test_http_route_table(self):
        port=self.port();binary=self.compile('import http;native main{http_serve_routing_mt('+str(port)+',2);}')
        self.assertEqual(json.loads(self.request(self.server(binary),port)),{'status':'ok'})
    def test_http_tls_if_available(self):
        features=self.compile('import http;native main{println(http_has_tls());return 0;}','features')
        if subprocess.check_output([str(features)],text=True).strip()!='1':self.skipTest('OpenSSL not enabled')
        cert=self.folder/'cert.pem';key=self.folder/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost','-keyout',str(key),'-out',str(cert)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=15)
        port=self.port();binary=self.compile('import http;native main{let s: int=http_listen_tls('+str(port)+',"'+str(cert)+'","'+str(key)+'");http_prepare(s,"tls ok");http_serve_tls_mt(s,2);}')
        self.assertEqual(self.request(self.server(binary),port,tls=True),b'tls ok')
    def test_lsp_initialize_shutdown(self):
        messages=[{'jsonrpc':'2.0','id':1,'method':'initialize','params':{'rootUri':None,'capabilities':{}}},{'jsonrpc':'2.0','id':2,'method':'shutdown','params':None},{'jsonrpc':'2.0','method':'exit'}]
        payload=b''.join(b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body for body in [json.dumps(m).encode() for m in messages])
        r=subprocess.run([config.lsp],input=payload,capture_output=True,timeout=10)
        self.assertEqual(r.returncode,0,r.stderr);out=r.stdout;responses=[]
        while out:
            header,out=out.split(b'\r\n\r\n',1);length=int(header.split(b':',1)[1]);responses.append(json.loads(out[:length]));out=out[length:]
        self.assertEqual([x['id'] for x in responses],[1,2]);self.assertIn('capabilities',responses[0]['result'])
    def test_nested_json_and_string_escapes(self):
        binary=self.compile(r'''import json;native main{println(json_get_path_int("{\"nested\":{\"value\":9007199254740993}}","nested.value"));println("literal\\n");println("actual\nnewline");return 0;}''')
        r=subprocess.run([str(binary)],capture_output=True,text=True,timeout=5)
        self.assertEqual(r.returncode,0,r.stderr);self.assertEqual(r.stdout,'9007199254740993\nliteral\\n\nactual\nnewline\n')

if __name__=='__main__':unittest.main()
