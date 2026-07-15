whpx_ss = ss.source_set()
whpx_ss.add(files(
  'whpx-accel-ops.c',
  # AEMU 'whpx-common.c'
))

specific_ss.add_all(when: 'CONFIG_WHPX', if_true: whpx_ss)
