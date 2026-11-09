"""Build a developer adapter with an in-place ANN mode switch in an isolated overlay."""
from pathlib import Path
import subprocess

overlay = Path('/v4src')
(overlay / 'include/minivectordb').mkdir(parents=True, exist_ok=True)
header = Path('/work/include/minivectordb/db.h').read_text()
needle = '  Result execute(const std::string& sql);'
assert header.count(needle) == 1
header = header.replace(needle, needle + '\n  void set_ann_mode(bool enabled);')
(overlay / 'include/minivectordb/db.h').write_text(header)
source = Path('/work/src/db.cpp').read_text()
needle = 'Database::~Database()=default;'
assert source.count(needle) == 1
source = source.replace(needle, needle + '\nvoid Database::set_ann_mode(bool enabled) { impl_->ann_mode=enabled; }')
(overlay / 'db.cpp').write_text(source)
adapter = Path('/work/tools/public_case_adapter.cpp').read_text()
needle = 'if (argc != 2)'
assert adapter.count(needle) == 1
adapter = adapter.replace(needle, 'if (argc < 2 || argc > 3)')
adapter = adapter.replace('mvdb::Database db(argv[1]);',
                          'mvdb::Database db(argv[1], argc == 3 && std::string(argv[2]) == "ann");')
needle = '      if (sql.empty()) continue;'
assert adapter.count(needle) == 1
adapter = adapter.replace(needle, needle + '''
      if (sql == "--mode ann" || sql == "--mode exact") {
        db.set_ann_mode(sql == "--mode ann");
        std::cout << "{\\\"driver\\\":\\\"mode\\\"}\\n" << std::flush;
        continue;
      }''')
(overlay / 'adapter.cpp').write_text(adapter)
subprocess.run(['g++', '-std=c++17', '-O3', '-DNDEBUG', '-I/v4src/include', '-I/work/include',
                '/v4src/db.cpp', '/work/src/disk_store.cpp', '/work/src/ivf_index.cpp',
                '/v4src/adapter.cpp', '-o', '/v4src/public_v4_adapter'], check=True)
