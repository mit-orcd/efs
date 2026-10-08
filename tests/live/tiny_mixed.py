"""Finite tiny CREATE/close storm overlapping bounded large-file overwrites."""
import concurrent.futures
import hashlib
import os
from pathlib import Path
import threading
import time


def body(worker,index):
    return hashlib.sha256(f'{worker}:{index}'.encode()).digest()*16


def exercise(mount,pids):
    folder=Path(mount)/'tiny-mixed';folder.mkdir()
    tiny_workers,large_workers,count=8,4,64
    gate=threading.Barrier(tiny_workers+large_workers)
    done=threading.Event();monitor_done=threading.Event()
    opened=[];closed=[];rss={str(pid):0 for pid in pids};samples=0
    def monitor():
        nonlocal samples
        while not monitor_done.wait(.05):
            for pid in pids:
                try:
                    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
                        if line.startswith('VmRSS:'):rss[str(pid)]=max(rss[str(pid)],int(line.split()[1]))
                except FileNotFoundError:raise AssertionError(f'fixture process {pid} exited')
            samples+=1
    def tiny(worker):
        gate.wait(timeout=30)
        for index in range(count):
            path=folder/f'tiny-{worker}-{index}'
            started=time.monotonic_ns();fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
            opened.append((time.monotonic_ns()-started)/1000)
            try:
                data=body(worker,index);assert os.write(fd,data)==len(data)
            finally:
                started=time.monotonic_ns();os.close(fd)
                closed.append((time.monotonic_ns()-started)/1000)
    def large(worker):
        data=hashlib.sha256(f'large-{worker}'.encode()).digest()*32768
        fd=os.open(folder/f'large-{worker}',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        blocks=0;gate.wait(timeout=30)
        try:
            while blocks<16 or not done.is_set():
                assert os.pwrite(fd,data,(blocks%16)*len(data))==len(data)
                blocks+=1
                if blocks%4==0:os.fsync(fd)
            os.fsync(fd)
        finally:os.close(fd)
        return blocks*len(data)
    started=time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=13) as pool:
        watcher=pool.submit(monitor)
        big=[pool.submit(large,i) for i in range(large_workers)]
        small=[pool.submit(tiny,i) for i in range(tiny_workers)]
        try:
            try:
                for future in small:future.result(timeout=180)
            finally:done.set()
            written=[future.result(timeout=180) for future in big]
        finally:monitor_done.set()
        watcher.result(timeout=5)
    def quantiles(values):
        values=sorted(values);assert len(values)==tiny_workers*count
        return {f'p{q}_us':values[min(len(values)-1,(len(values)*q+99)//100-1)] for q in (50,95,99)}|{'max_us':values[-1]}
    return {'tiny_files':tiny_workers*count,'tiny_bytes':512,'large_files':large_workers,
            'large_live_bytes_each':16<<20,'large_written_bytes':written,
            'wall_s':time.monotonic()-started,'open':quantiles(opened),'close':quantiles(closed),
            'peak_rss_kib':rss,'rss_samples':samples}


def verify_and_delete(mount):
    folder=Path(mount)/'tiny-mixed'
    for worker in range(8):
        for index in range(64):
            path=folder/f'tiny-{worker}-{index}';assert path.read_bytes()==body(worker,index),path
            path.unlink()
    for worker in range(4):
        path=folder/f'large-{worker}'
        expected=hashlib.sha256(f'large-{worker}'.encode()).digest()*32768
        with path.open('rb') as stream:
            for _ in range(16):assert stream.read(len(expected))==expected,path
            assert not stream.read(1),path
        path.unlink()
    folder.rmdir()
