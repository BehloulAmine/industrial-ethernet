set(OPENER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/opener/source/src)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${OPENER_ROOT}
  ${OPENER_ROOT}/cip
  ${OPENER_ROOT}/enet_encap
  ${OPENER_ROOT}/ports
  ${OPENER_ROOT}/utils
)

target_compile_definitions(app PRIVATE
  PC_OPENER_ETHERNET_BUFFER_SIZE=512
  OPENER_CONSUMED_DATA_HAS_RUN_IDLE_HEADER
  RESTRICT=restrict
  MSG_NOSIGNAL=0
)

target_sources(app PRIVATE
  app_eip.c
  opener_zephyr_port.c

  ${OPENER_ROOT}/cip/appcontype.c
  ${OPENER_ROOT}/cip/cipassembly.c
  ${OPENER_ROOT}/cip/cipclass3connection.c
  ${OPENER_ROOT}/cip/cipcommon.c
  ${OPENER_ROOT}/cip/cipconnectionmanager.c
  ${OPENER_ROOT}/cip/cipconnectionobject.c
  ${OPENER_ROOT}/cip/cipdlr.c
  ${OPENER_ROOT}/cip/cipelectronickey.c
  ${OPENER_ROOT}/cip/cipepath.c
  ${OPENER_ROOT}/cip/cipethernetlink.c
  ${OPENER_ROOT}/cip/cipidentity.c
  ${OPENER_ROOT}/cip/cipioconnection.c
  ${OPENER_ROOT}/cip/cipmessagerouter.c
  ${OPENER_ROOT}/cip/cipqos.c
  ${OPENER_ROOT}/cip/cipstring.c
  ${OPENER_ROOT}/cip/cipstringi.c
  ${OPENER_ROOT}/cip/ciptcpipinterface.c
  ${OPENER_ROOT}/cip/ciptypes.c

  ${OPENER_ROOT}/enet_encap/cpf.c
  ${OPENER_ROOT}/enet_encap/encap.c
  ${OPENER_ROOT}/enet_encap/endianconv.c

  ${OPENER_ROOT}/ports/generic_networkhandler.c
  ${OPENER_ROOT}/ports/socket_timer.c

  ${OPENER_ROOT}/utils/doublylinkedlist.c
  ${OPENER_ROOT}/utils/enipmessage.c
  ${OPENER_ROOT}/utils/random.c
  ${OPENER_ROOT}/utils/xorshiftrandom.c
)
