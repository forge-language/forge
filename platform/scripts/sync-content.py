#!/usr/bin/env python3
"""Sync checked-in public content snapshots into Vite's static directory."""
import pathlib,shutil
ROOT=pathlib.Path(__file__).resolve().parents[1]
for kind in ['project','reports']:
    source=ROOT/'content'/kind
    target=ROOT/'frontend/public'/kind
    target.mkdir(parents=True,exist_ok=True)
    shutil.copytree(source,target,dirs_exist_ok=True)
print('Synced public project documents and performance evidence')
