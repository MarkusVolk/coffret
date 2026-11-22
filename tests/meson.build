foreach name : ['clone_plan', 'disk_image', 'domain_xml', 'frame_store', 'keymap', 'snapshot_tree']
  test(name, executable(name + '-test', name + '_test.cpp', dependencies: [core_dep]))
endforeach
