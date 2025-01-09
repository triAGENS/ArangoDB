"""Async Registry Pretty Printer for gdb

Parsing 

"""

# class Promise:
#     def __init__(self, value):

# class ThreadRegistry:
#     def __init__(self, value):
#        self.head = value["promise_head"]

#     def to_string(self):
#         return self.head
        
    
class ThreadRegistries:
    def __init__(self, value):
        self.begin = value["_M_start"]
        self.end = value["_M_finish"]
        
    def __iter__(self):
        counter = 0
        current = self.begin
        while current != self.end:
            registry = ("[%d]" % counter, current.dereference()["_M_ptr"].dereference())
            current += 1
            counter += 1
            yield registry

class AsyncRegistry:
    def __init__(self, value):
        self.thread_registries = value['registries']
        self.stacktraces = []

    def to_string (self):
        count = 0
        for registry in ThreadRegistries(self.thread_registries["_M_impl"]):
            # TODO collect all promises in a forest
            count += 1
            if count % 2 == 0:
                self.stacktraces.append(registry);
        return "bla_vector " + str(count)

    def children(self):
        # TODO create stacktraces from the forest and yield each one
        for trace in self.stacktraces:
            # keep in mind: this needs to yield a tuple (not sure if this is what we want for the stacktrace)
            yield trace

def lookup_type (val):
    if str(val.type) == 'arangodb::async_registry::Registry':
        return AsyncRegistry(val)
    return None

gdb.pretty_printers.append (lookup_type)
