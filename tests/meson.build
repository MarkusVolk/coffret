foreach name : ['answer_file', 'clone_plan', 'disk_image', 'domain_xml', 'frame_store', 'keymap', 'snapshot_tree', 'usb_devices']
  test(name, executable(name + '-test', name + '_test.cpp', dependencies: [core_dep]))
endforeach
