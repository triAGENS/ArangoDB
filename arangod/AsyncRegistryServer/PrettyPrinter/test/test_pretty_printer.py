class RequesterPrinter:
    def __init__(self, value):
        self.alternative = value['_M_index']
        if self.alternative == 0:
            self.content = value['_M_u']['_M_first']['_M_storage']
        elif self.alternative == 1:
           self.content = value['_M_u']['_M_rest']['_M_first']['_M_storage']
        elif self.alternative == 2:
           self.content = value['_M_u']['_M_rest']['_M_rest']['_M_first']['_M_storage']
        else:
            return "wrong input"

    def __str__(self):
        return "(" + str(self.alternative) + ", " + str(self.content) + ")"

class PromisePrinter:
    def __init__(self, value_ptr):
        self.identification = value_ptr
        self.requester = value_ptr.dereference()['requester']['_M_i']
        self.next_promise = value_ptr.dereference()['next']

    def __str__(self):
        return "id " + str(self.identification) + ": requ " + str(RequesterPrinter(self.requester)) + ", next " + str(self.next_promise)

class ListPrinter:
    def __init__(self,value):
        self.head = value['head']

    def to_string(self):
        return str(PromisePrinter(self.head['_M_b']['_M_p']))

def lookup_type (val):
    if str(val.type) == 'PromiseList':
        return ListPrinter(val)
    return None
        
gdb.pretty_printers.append (lookup_type)
