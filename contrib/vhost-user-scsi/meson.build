if libiscsi.found()
  executable('vhost-user-scsi', files('vhost-user-scsi.c'),
             dependencies: [qemuutil, print_stub, libiscsi, vhost_user],
             build_by_default: host_os == 'linux',
             install: false)
endif
