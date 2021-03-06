#include "measure_throughput_fd.h"

static bool go_exit = false;

int main(int argc, char *argv[]) {

  char source_module[] = "MODULE_MAC";
  char target_module[] = "MODULE_PHY";
  tput_context_t tput_context;

  // Initialize signal handler.
  initialize_signal_handler();

  // Retrieve input arguments.
  parse_args(&tput_context.args, argc, argv);

  // Create communicator.
  communicator_make(source_module, target_module, NULL, &tput_context.handle);

  // Wait some time before starting anything.
  sleep(2);

  // Start Rx thread.
  printf("[Main] Starting Rx side thread...\n");
  start_rx_side_thread(&tput_context);

  // Run Tx side loop.
  printf("[Main] Starting Tx side...\n");
  tx_side(&tput_context);

  // Wait some time so that all packets are transmitted.
  printf("[Main] Leaving the application in 2 seconds...\n");
  sleep(2);

  // Stop Rx side thread.
  stop_rx_side_thread(&tput_context);

  // Free communicator related objects.
  communicator_free(&tput_context.handle);

  return 0;
}

void *rx_side(void *h) {
  tput_context_t *tput_context = (tput_context_t*)h;
  basic_ctrl_t basic_ctrl;
  phy_stat_t phy_rx_stat;
  uchar data[10000];
  bool ret, is_1st_packet = true;
  uint64_t nof_rec_bytes = 0, tput_avg_cnt = 0;
  struct timespec time_1st_packet;
  double time_diff = 0.0;
  double tput = 0, tput_avg = 0;
  double tput_interval = ((double)tput_context->args.interval)*1000.0;
  uint32_t phy_id = 0;

  // Assign address of data vector to PHY Rx stat structure.
  phy_rx_stat.stat.rx_stat.data = data;

  // Set priority to RX thread.
  uhd_set_thread_priority(1.0, true);

  clock_gettime(CLOCK_REALTIME, &time_1st_packet);

  // Create basic control message to control Tx chain.
  createBasicControl(&basic_ctrl, PHY_RX_ST, phy_id, 66, tput_context->args.phy_bw_idx, tput_context->args.rx_channel, 0, 0, tput_context->args.rx_gain, 1, NULL);

  // Send RX control to PHY.
  communicator_send_basic_control(tput_context->handle, &basic_ctrl);

  // Loop until otherwise said.
  printf("[Rx side] Starting Rx side thread loop...\n");
  while(tput_context->run_rx_side_thread) {

    // Retrieve message sent by PHY.
    // Try to retrieve a message from the QUEUE. It waits for a specified amount of time before timing out.
    ret = communicator_get_high_queue_wait_for(tput_context->handle, 500, (void * const)&phy_rx_stat, NULL);

    // If message is properly retrieved and parsed, then relay it to the correct module.
    if(tput_context->run_rx_side_thread && ret) {

      if(phy_rx_stat.status == PHY_SUCCESS) {

        if(phy_rx_stat.seq_number == 66) {

          nof_rec_bytes += phy_rx_stat.stat.rx_stat.length;

          if(is_1st_packet) {
            is_1st_packet = false;
            clock_gettime(CLOCK_REALTIME, &time_1st_packet);
          } else {
            time_diff = profiling_diff_time(&time_1st_packet);

            if(time_diff >= tput_interval) {

              tput = (nof_rec_bytes*8)/(time_diff/1000.0);

              tput_avg += tput;
              tput_avg_cnt++;

              is_1st_packet = true;
              nof_rec_bytes = 0;

              printf("[Rx side] Local tput: %f [bps]\n",tput);
            }
          }
        }
      }
    }
  }
  printf("[Rx side] Average tput: %f [bps]\n",tput_avg/tput_avg_cnt);
  printf("[Rx side] Leaving Rx side thread.\n",0);
  // Exit thread with result code.
  pthread_exit(NULL);
}

void tx_side(tput_context_t *tput_context) {
  basic_ctrl_t basic_ctrl;
  int64_t packet_cnt = 0;
  unsigned int tb_size = srslte_ra_get_tb_size_scatter((tput_context->args.phy_bw_idx-1), tput_context->args.mcs);
  int numOfBytes = tput_context->args.nof_slots_to_tx*tb_size;
  // Create some data.
  uchar data[numOfBytes];
  generateData(numOfBytes, data);
  uint32_t phy_id = 0;

  // Set priority to RX thread.
  uhd_set_thread_priority(1.0, true);

  uint64_t time_offset = (uint64_t)(tput_context->args.nof_slots_to_tx*1000.0 + 500.0);

  printf("[Tx side] timestamp offset: %" PRIu64 "\n", time_offset);

  // Create basic control message to control Tx chain.
  createBasicControl(&basic_ctrl, PHY_TX_ST, phy_id, 66, tput_context->args.phy_bw_idx, tput_context->args.tx_channel, 0, tput_context->args.mcs, tput_context->args.tx_gain, numOfBytes, data);

  // Retrieve current time from host PC.
  basic_ctrl.timestamp = get_host_time_now_us();

  //printf("time now: %" PRIu64 "\n", basic_ctrl.timestamp);

  while(!go_exit && packet_cnt != tput_context->args.nof_packets_to_tx) {

    // Add some time to the current time.
    basic_ctrl.timestamp = basic_ctrl.timestamp + time_offset;

    //printf("timestamp: %" PRIu64 "\n", basic_ctrl.timestamp);

    communicator_send_basic_control(tput_context->handle, &basic_ctrl);

    usleep(900*tput_context->args.nof_slots_to_tx);

    packet_cnt++;
  }
}

void sig_int_handler(int signo) {
  if(signo == SIGINT) {
    go_exit = true;
    printf("SIGINT received. Exiting...\n",0);
  }
}

void initialize_signal_handler() {
  sigset_t sigset;
  sigemptyset(&sigset);
  sigaddset(&sigset, SIGINT);
  sigprocmask(SIG_UNBLOCK, &sigset, NULL);
  signal(SIGINT, sig_int_handler);
}

void createBasicControl(basic_ctrl_t *basic_ctrl, trx_flag_e trx_flag, uint32_t phy_id, uint64_t seq_num, uint32_t bw_idx, uint32_t ch, uint64_t timestamp, uint32_t mcs, int32_t gain, uint32_t length, uchar *data) {
  basic_ctrl->trx_flag = trx_flag;
  basic_ctrl->phy_id = phy_id;
  basic_ctrl->seq_number = seq_num;
  basic_ctrl->bw_idx = bw_idx;
  basic_ctrl->ch = ch;
  basic_ctrl->timestamp = timestamp;
  basic_ctrl->mcs = mcs;
  basic_ctrl->gain = gain;
  basic_ctrl->length = length;
  if(data == NULL) {
    basic_ctrl->data = NULL;
  } else {
    basic_ctrl->data = data;
  }
}

void default_args(tput_test_args_t *args) {
  args->phy_bw_idx = BW_IDX_Five;
  args->mcs = 0;
  args->rx_channel = 0;
  args->rx_gain = 10;
  args->tx_channel = 0;
  args->tx_gain = 10;
  args->tx_side = true;
  args->nof_slots_to_tx = 1;
  args->interval = 10;
  args->nof_packets_to_tx = -1;
}

int start_rx_side_thread(tput_context_t *tput_context) {
  // Enable Rx side thread.
  tput_context->run_rx_side_thread = true;
  // Create thread attr and Id.
  pthread_attr_init(&tput_context->rx_side_thread_attr);
  pthread_attr_setdetachstate(&tput_context->rx_side_thread_attr, PTHREAD_CREATE_JOINABLE);
  // Create thread to sense channel.
  int rc = pthread_create(&tput_context->rx_side_thread_id, &tput_context->rx_side_thread_attr, rx_side, (void *)tput_context);
  if(rc) {
    printf("[Rx side] Return code from Rx side pthread_create() is %d\n", rc);
    return -1;
  }
  return 0;
}

int stop_rx_side_thread(tput_context_t *tput_context) {
  tput_context->run_rx_side_thread = false; // Stop Rx side thread.
  pthread_attr_destroy(&tput_context->rx_side_thread_attr);
  int rc = pthread_join(tput_context->rx_side_thread_id, NULL);
  if(rc) {
    printf("[Rx side] Return code from Rx side pthread_join() is %d\n", rc);
    return -1;
  }
  return 0;
}

// This function returns timestamp with microseconds precision.
inline uint64_t get_host_time_now_us() {
  struct timespec host_timestamp;
  // Retrieve current time from host PC.
  clock_gettime(CLOCK_REALTIME, &host_timestamp);
  return (uint64_t)(host_timestamp.tv_sec*1000000LL) + (uint64_t)((double)host_timestamp.tv_nsec/1000LL);
}

inline double profiling_diff_time(struct timespec *timestart) {
  struct timespec timeend;
  clock_gettime(CLOCK_REALTIME, &timeend);
  return time_diff(timestart, &timeend);
}

inline double time_diff(struct timespec *start, struct timespec *stop) {
  struct timespec diff;
  if(stop->tv_nsec < start->tv_nsec) {
    diff.tv_sec = stop->tv_sec - start->tv_sec - 1;
    diff.tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000;
  } else {
    diff.tv_sec = stop->tv_sec - start->tv_sec;
    diff.tv_nsec = stop->tv_nsec - start->tv_nsec;
  }
  return (double)(diff.tv_sec*1000) + (double)(diff.tv_nsec/1.0e6);
}

void parse_args(tput_test_args_t *args, int argc, char **argv) {
  int opt;
  default_args(args);
  while((opt = getopt(argc, argv, "bgikmnprst0123456789")) != -1) {
    switch(opt) {
    case 'b':
      args->tx_gain = atoi(argv[optind]);
      printf("[Input argument] Tx gain: %d\n", args->tx_gain);
      break;
    case 'g':
      args->rx_gain = atoi(argv[optind]);
      printf("[Input argument] Rx gain: %d\n", args->rx_gain);
      break;
    case 'i':
      args->interval = atoi(argv[optind]);
      printf("[Input argument] Tput interval: %d\n", args->interval);
      break;
    case 'k':
      args->nof_packets_to_tx = atoi(argv[optind]);
      printf("[Input argument] Number of packets to transmit: %d\n", args->nof_packets_to_tx);
      break;
    case 'm':
      args->mcs = atoi(argv[optind]);
      printf("[Input argument] MCS: %d\n", args->mcs);
      break;
    case 'n':
      args->nof_slots_to_tx = atoi(argv[optind]);
      printf("[Input argument] Number of consecutive slots to be transmitted: %d\n", args->nof_slots_to_tx);
      break;
    case 'p':
      args->phy_bw_idx = helpers_get_bw_index_from_prb(atoi(argv[optind]));
      printf("[Input argument] PHY BW in PRB: %d - Mapped index: %d\n", atoi(argv[optind]), args->phy_bw_idx);
      break;
    case 'r':
      args->rx_channel = atoi(argv[optind]);
      printf("[Input argument] Rx channel: %d\n", args->rx_channel);
      break;
    case 's':
      args->tx_side = false;
      printf("[Input argument] Tx side: %d\n", args->tx_side);
      break;
    case 't':
      args->tx_channel = atoi(argv[optind]);
      printf("[Input argument] Tx channel: %d\n", args->tx_channel);
      break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      break;
    default:
      printf("Error parsing arguments...\n");
      exit(-1);
    }
  }
}

void generateData(uint32_t numOfBytes, uchar *data) {
  // Create some data.
  printf("Creating %d data bytes\n",numOfBytes);
  for(int i = 0; i < numOfBytes; i++) {
    data[i] = (uchar)(rand() % 256);
  }
}
