#!/usr/bin/env python3

"""Async Registry Pretty Printer

This script allows a user to print the async stacktraces from server_url/_admin/async-registry as a list of trees.

Usage: curl -s server_url/_admin/async-registry <root-user>:<password> | ./pretty_printer.py

"""

import sys
import json
from typing import List
from typing import Optional

class Thread(object):
    def __init__(self, name: str, id: int):
        self.name = name
        self.id = id
    @classmethod
    def from_json(cls, blob: dict):
        return cls(blob["name"], blob["LWPID"])
    def __str__(self):
        return self.name + "(" + str(self.id) + ")"

class SourceLocation(object):
    def __init__(self, file_name: str, line: int, function_name: str):
        self.file_name = file_name
        self.line = line
        self.function_name = function_name
    @classmethod
    def from_json(cls, blob: dict):
        return cls(blob["file_name"], blob["line"], blob["function_name"])
    def __str__(self):
        return self.function_name + " (" + self.file_name + ":" + str(self.line) + ")"
    
class Requester(object):
    def __init__(self, is_sync: bool, item: int):
        self.is_sync = is_sync
        self.item = item
    @classmethod
    def from_json(cls, blob: dict):
        if not blob:
            return None
        sync = blob.get("thread")
        if sync is not None:
            return cls(True, sync)
        else:
            return cls(False, blob["promise"])
    def __str__(self):
        if self.is_sync:
            # a sync requester is always at the bottom of a tree,
            # but has no entry on its own,
            # therefore just add it here in a new line
            return "\n" + "─ " + str(Thread.from_json(self.item))
        else:
            return ""

class Data(object):
    def __init__(self, owning_thread: Thread, source_location: SourceLocation, id: int, state: str, requester: Requester):
        self.owning_thread = owning_thread
        self.source_location = source_location
        self.id = id
        self.waiter = requester
        self.state = state
    @classmethod
    def from_json(cls, blob: dict):
        return cls(Thread.from_json(blob["owning_thread"]), SourceLocation.from_json(blob["source_location"]), blob["id"], blob["state"], Requester.from_json(blob["requester"]))
    def __str__(self):
        waiter_str = str(self.waiter) if self.waiter != None else ""
        return str(self.source_location) + ", " + str(self.owning_thread) + ", " + self.state + waiter_str
        
class Promise(object):
    def __init__(self, hierarchy: int, data: Data):
        self.hierarchy = hierarchy
        self.data = data
    
def branching_symbol(hierarchy:int, previous_hierarchy: Optional[int]) -> str:
    if hierarchy == previous_hierarchy:
        return "├"
    else:
        return "┌"
    
def branch_ascii(hierarchy: int, continuations: list) -> str:
    ascii = [" " for x in range(2*hierarchy+2)] + [branching_symbol(hierarchy, continuations[-1] if len(continuations) > 0 else None)] + [" "]
    for continuation in continuations:
        if continuation < hierarchy:
            ascii[2*continuation] = "│"
    return ''.join(ascii)
                
    
class Stacktrace(object):
    def __init__(self, promises: List[dict]):
        self.promises = promises
    def __str__(self):
        lines = []
        stack = []
        for promise in self.promises:
            hierarchy = promise["hierarchy"]
            # pop finished hierarchy
            if len(stack) > 0 and hierarchy < stack[-1]:
                stack.pop()

            ascii = branch_ascii(hierarchy, stack)

            # push current but not if already on stack
            if len(stack) == 0 or hierarchy != stack[0]:
                stack.append(hierarchy)

            lines.append(ascii + str(Data.from_json(promise["data"])))
        return "\n".join(lines)
    
def main():
    string = sys.stdin.read()
    data = json.loads(string)["promise_stacktraces"]
    print("\n\n".join(str(Stacktrace(**{"promises": x})) for x in data))
    

if __name__ == '__main__':
    main()
