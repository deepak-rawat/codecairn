class Counter:
    def __init__(self, value=0):
        self.value = value

    def increment(self, amount=1):
        self.value += amount
        return self.value
