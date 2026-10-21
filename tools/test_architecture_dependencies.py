"""Fault-test dependency checking in temporary project trees, without SDKs."""
from pathlib import Path
import tempfile
import unittest
from check_architecture_dependencies import include_graph, violations


class Dependencies(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, text=''):
        path = self.root/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def check(self):
        return violations(include_graph(self.root))

    def test_direct_relative_and_angle_imports(self):
        self.write('include/ui/view.h')
        self.write('include/apps/app.h')
        self.write('include/nav_handler.h')
        self.write('include/services/bad.h', '#include "../ui/view.h"\n')
        self.write('src/sfx/bad.cpp', '#include <apps/app.h>\n')
        self.write('src/core/bad.cpp', '#include "nav_handler.h"\n')
        self.assertEqual(len(self.check()), 3)

    def test_transitive_cycle_and_diagnostic_line(self):
        self.write('src/services/worker.cpp', '// comment\n\n#include "bridge.h"\n')
        self.write('include/bridge.h', '#include "cycle.h"\n/* disabled-looking text */\n#include "scene/screen.h"\n')
        self.write('include/cycle.h', '#include "bridge.h"\n')
        self.write('include/scene/screen.h')
        failures = self.check()
        self.assertEqual(len(failures), 1)
        origin, chain = failures[0]
        self.assertEqual(origin.as_posix(), 'src/services/worker.cpp')
        self.assertEqual([edge.line for edge in chain], [3, 3])
        self.assertEqual(chain[-1].target.as_posix(), 'include/scene/screen.h')

    def test_comments_and_embedded_source_are_not_includes(self):
        self.write('include/app.h')
        self.write('src/core/ok.cpp', '''// #include "app.h"
/*
#include "app.h"
*/
const char* url="https://example.test/*not-a-comment*/";
const char* sample=R"sample(
#include "app.h"
)sample";
const char* escaped="\\\" // /*";
''')
        self.assertEqual(self.check(), [])
        self.write('src/core/ok.cpp', (self.root/'src/core/ok.cpp').read_text()+'#include "app.h"\n')
        self.assertEqual(self.check()[0][1][0].line, 10)

    def test_core_cannot_reach_service_through_neutral_header(self):
        self.write('src/core/model.cpp', '#include "bridge.h"\n')
        self.write('include/bridge.h', '#include "services/worker.h"\n')
        self.write('include/services/worker.h')
        self.assertEqual(len(self.check()), 1)

    def test_operational_widgets_and_downward_imports_allowed(self):
        self.write('src/apps/app.cpp', '#include "services/session.h"\n')
        self.write('include/services/session.h', '#include "core/model.h"\n#include "widgets/launch.h"\n')
        self.write('include/core/model.h', '#include <stdint.h>\n#include "hal/device.h"\n')
        self.write('include/hal/device.h')
        self.write('include/widgets/launch.h', '#include <stddef.h>\n')
        self.assertEqual(self.check(), [])
        self.write('include/widgets/launch.h', '#include "app.h"\n')
        self.write('include/app.h')
        self.assertEqual(len(self.check()), 1)

    def test_all_conditional_branches_checked(self):
        self.write('src/services/example.cpp', '#if 0\n#include "ui/view.h"\n#endif\n')
        self.write('include/ui/view.h')
        self.assertEqual(len(self.check()), 1)

    def test_core_cannot_import_game_presentation(self):
        self.write('src/core/physics.cpp', '#include "ui/golf_draw.h"\n')
        self.write('include/ui/golf_draw.h')
        self.assertEqual(len(self.check()), 1)


if __name__ == '__main__':
    unittest.main()
