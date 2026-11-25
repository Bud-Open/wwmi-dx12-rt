import pathlib
import runpy
import sys
sys.path.insert(0, str(pathlib.Path.cwd()))
import glad.files
Base = glad.files.StaticFileOpener
class PinnedLocalOpener(Base):
    def urlopen(self, url, data=None, *args, **kwargs):
        if data is None and url.startswith("file:"):
            return open(url[5:], "rb")
        return super().urlopen(url, data, *args, **kwargs)
glad.files.StaticFileOpener = PinnedLocalOpener
runpy.run_module("glad", run_name="__main__")
