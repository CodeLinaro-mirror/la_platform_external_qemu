test_plugins = [
'bb.c',
'discons.c',
'empty.c',
'inline.c',
'insn.c',
'mem.c',
'patch.c',
'registers.c',
'reset.c',
'setpc.c',
'syscall.c',
]

t = []
if get_option('plugins')
  foreach i : test_plugins
    t += shared_module(fs.stem(i), files(i),
                       dependencies: plugins_deps)
  endforeach
endif
if t.length() > 0
  alias_target('test-plugins', t)
else
  true_cmd = find_program('../../../scripts/true.py')
  run_target('test-plugins', command: true_cmd)
endif

plugin_modules += t
