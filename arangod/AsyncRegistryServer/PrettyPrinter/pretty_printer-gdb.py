"""Async Registry Pretty Printer for gdb

Parsing 

"""

class Requester:
    def __init__(self, value):
        self.alternative = value['_M_index'] # this is currently 0
        self.value = value['_M_u']['_M_rest']['_M_first']['_M_storage'] # this is currently type void* so probably as PromiseId

    def __str__(self):
        return str(self.alternative) + ": " + str(self.value)

class Promise:
    def __init__(self, value_ptr):
        self.id = value_ptr.address # TODO seems to be incorrect id
        value = value_ptr.dereference()
        self.thread = value["thread"] # TODO add type
        self.source_location = value["source_location"] # TODO add type
        self.requester = Requester(value["requester"]['_M_i']) # TODO add type
        self.state = value["state"] # TODO add type

    def __str__(self):
        return str(self.id) + ": " + str(self.state) + " - " + str(self.requester)

class PromiseList:
    def __init__(self, value_ptr):
        self.head_ptr = value_ptr

    def __iter__(self):
        current = self.head_ptr
        next = current
        while next != 0:
            current = next
            next = current.dereference()["next"]
            yield Promise(current)

class ThreadRegistry:
    def __init__(self, value):
       self.promises = PromiseList(value["promise_head"]["_M_b"]["_M_p"])

    def __str__(self):
        for promise in self.promises:
            return str(promise)
    
class ThreadRegistryList:
    def __init__(self, value):
        self.begin = value["_M_start"]
        self.end = value["_M_finish"]
        
    def __iter__(self):
        counter = 0
        current = self.begin
        while current != self.end:
            thread_registry = ThreadRegistry(current.dereference()["_M_ptr"].dereference())
            output = ("[%d]" % counter, str(thread_registry))
            current += 1
            counter += 1
            yield output

class AsyncRegistry:
    def __init__(self, value):
        self.thread_registries = value['registries']
        self.stacktraces = []

    def to_string (self):
        count = 0
        for registry in ThreadRegistryList(self.thread_registries["_M_impl"]):
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
