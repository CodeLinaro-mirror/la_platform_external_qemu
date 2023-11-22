executable('vhost-user-blk', files('vhost-user-blk.c'),
           dependencies: [qemuutil, print_stub, vhost_user],
           build_by_default: host_os == 'linux',
           install: false)
