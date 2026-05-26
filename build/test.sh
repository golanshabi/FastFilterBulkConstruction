# name of the FIFOs
FIFO_PREFIX="perf_fd"

# remove dangling files if any
rm -rf ${FIFO_PREFIX}.*

# create two fifos
mkfifo ${FIFO_PREFIX}.ctl
mkfifo ${FIFO_PREFIX}.ack

# associate file descriptors
exec {perf_ctl_fd}<>${FIFO_PREFIX}.ctl
exec {perf_ack_fd}<>${FIFO_PREFIX}.ack

# set env vars for application
export PERF_CTL_FD=${perf_ctl_fd}
export PERF_ACK_FD=${perf_ack_fd}

