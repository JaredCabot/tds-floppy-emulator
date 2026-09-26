"""locktest.py - hold the bench lock for N seconds (tests benchlock).
Usage: python tools/locktest.py 8"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import benchlock
benchlock.acquire("locktest")
time.sleep(float(sys.argv[1]) if len(sys.argv) > 1 else 8)
