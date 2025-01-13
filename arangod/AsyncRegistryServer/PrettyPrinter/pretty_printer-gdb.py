"""Async Registry Pretty Printer for gdb

Parsing 

"""

class State:
    def __init__(self, value):
        self.name = value['_M_i']

    def is_deleted(self):
        return str(self.name) == "arangodb::async_registry::State::Deleted"

    def __str__(self):
        return str(self.name)
    
class Thread:
    def __init__(self, value):
        # TODO is there a way to get the thread name?
        self.posix_id = value['posix_id']
        self.lwpid = value['kernel_id']

    def __str__(self):
        return "thread LWPID: " + str(self.lwpid) + " & posix_id: " + str(self.posix_id)

class PromiseId:
    def __init__(self, value):
       self.id = value 

    def __str__(self):
        return str(self.id)

class Requester:
    def __init__(self, value):
        self.value = value
        self.alternative = value['_M_index']
        if self.alternative == 0:
            self.content = Thread(value['_M_u']['_M_first']['_M_storage'])
        elif self.alternative == 1:
            self.content = PromiseId(value['_M_u']['_M_rest']['_M_first']['_M_storage'])
        else:
            self.content = None

    def is_valid(self):
        return self.content != None

    def __str__(self):
        return str(self.content)

class SourceLocation:
    def __init__(self, value):
        self.file_name = value['file_name']
        self.function_name = value['function_name']
        self.line = value['line']['_M_i']

    def __str__(self):
        return str(self.file_name) + ":" + str(self.line) + " " + str(self.function_name)

class Promise:
    # raw print in gdb: print /r *(registry.registries[0]._M_ptr).promise_head._M_b._M_p
    
    def __init__(self, value_ptr):
        self.id = value_ptr
        value = value_ptr.dereference()
        self.thread = Thread(value["thread"])
        self.source_location = SourceLocation(value["source_location"])
        self.requester = Requester(value["requester"]['_M_i'])
        self.state = State(value["state"])

    def is_valid(self):
        return not self.state.is_deleted() and self.requester.is_valid()

    def __str__(self):
        return str(self.id) + ": waiter " + str(self.requester) + " " + str(self.source_location) + ", " + str(self.thread) + ", " + str(self.state) 

class PromiseList:
    def __init__(self, begin):
        self.head_ptr = begin

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
        return "\n".join(str(promise) for promise in filter(lambda x: x.is_valid(), self.promises))
    
class ThreadRegistryList:
    def __init__(self, value):
        self.begin = value["_M_start"]
        self.end = value["_M_finish"]
        
    def __iter__(self):
        current = self.begin
        while current != self.end:
            registry = ThreadRegistry(current.dereference()["_M_ptr"].dereference())
            current += 1
            yield registry

class AsyncRegistry:
    def __init__(self, value):
        self.thread_registries = value['registries']
        self.stacktraces = []

    def to_string (self):
        count = 0
        for registry in ThreadRegistryList(self.thread_registries["_M_impl"]):
            # TODO collect all promises in a forest
            count += 1
            self.stacktraces.append(registry);
        return "async_registry on " + str(count) + " threads"

    def children(self):
        # TODO create stacktraces from the forest and yield each one
        counter = 0
        for trace in self.stacktraces:
            output = ("[%d]" % counter, str(trace))
            counter += 1
            # keep in mind: this needs to yield a tuple (not sure if this is what we want for the stacktrace)
            yield output

def lookup_type (val):
    if str(val.type) == 'arangodb::async_registry::Registry':
        return AsyncRegistry(val)
    return None

gdb.pretty_printers.append (lookup_type)
