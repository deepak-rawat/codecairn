# café 🧭
class Worker:
    async def shared(self):
        def nested():
            return 1
        local = nested()
        return local
