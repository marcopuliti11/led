/*****************************************************************************
 *
 *  $Id$
 *
 *  Copyright (C) 2007-2009  Florian Pose, Ingenieurgemeinschaft IgH
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *  ---
 *
 *  The license mentioned above concerns the source code only. Using the
 *  EtherCAT technology and brand is only permitted in compliance with the
 *  industrial property and similar rights of Beckhoff Automation GmbH.
 *
 ****************************************************************************/

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/****************************************************************************/

#include "ecrt.h"
#include "slaves.h" // generated with the "ethercat cstruct" command

/****************************************************************************/

// Application parameters
#define FREQUENCY 100
#define PRIORITY 0

// Optional features
#define CONFIGURE_PDOS  1
#define SDO_ACCESS      0

/****************************************************************************/

// EtherCAT
static ec_master_t *master = NULL;
static ec_master_state_t master_state = {};

static ec_domain_t *domain1 = NULL;
static ec_domain_state_t domain1_state = {};

static ec_slave_config_t *slave1_out1 = NULL;
static ec_slave_config_state_t slave1_out1_state = {};

// Timer
static unsigned int sig_alarms = 0;
static unsigned int user_alarms = 0;

/****************************************************************************/

// process data
static uint8_t *domain1_pd = NULL;

#define BusCouplerPos  0, 0
#define Slave1Pos 0, 1
#define Slave2Pos 0, 2
#define Slave3Pos 0, 3

// EK1100 | EtherCAT Coupler
#define Beckhoff_EK1100 0x00000002, 0x044c2c52

// EL2202 | 2-channel digital output terminal 24 V DC, TON/TOFF 1 µs, push-pull
#define Beckhoff_EL2202 0x00000002, 0x089a3052

// EL1252 | 2-channel digital input terminal with time stamp
#define Beckhoff_EL1252 0x00000002, 0x04e43052

// EL2252 | 2-channel digital output terminal with time stamp, tri-state
#define Beckhoff_EL2252 0x00000002, 0x08cc3052

// offsets for PDO entries
static unsigned int off_slave1_out1;
static unsigned int off_slave1_tristate1;
static unsigned int off_slave1_out2;
static unsigned int off_slave1_tristate2;

const static ec_pdo_entry_reg_t domain1_regs[] = {
    {Slave1Pos, Beckhoff_EL2202, 0x7000, 0x01, &off_slave1_out1},
//    {Slave1Pos, Beckhoff_EL2202, 0x7000, 0x02, &off_slave1_tristate1},
//    {Slave1Pos, Beckhoff_EL2202, 0x7010, 0x01, &off_slave1_out2},
//    {Slave1Pos, Beckhoff_EL2202, 0x7010, 0x02, &off_slave1_tristate2},

//    {Slave2Pos, Beckhoff_EL1252, ...},
//    {0, 3, Beckhoff_EL1252, ...},
//    {0, 4, Beckhoff_EL2252, ...},
    {}
};

static unsigned int counter = 0;
static unsigned int blink = 0;


#if SDO_ACCESS
static ec_sdo_request_t *sdo;
#endif

/*****************************************************************************/

void check_domain1_state(void)
{
    
    ec_domain_state_t ds;

    ecrt_domain_state(domain1, &ds);

    if (ds.working_counter != domain1_state.working_counter)
        printf("Domain1: WC %u.\n", ds.working_counter);
    if (ds.wc_state != domain1_state.wc_state)
        printf("Domain1: State %u.\n", ds.wc_state);

    domain1_state = ds;
}

/*****************************************************************************/

void check_master_state(void)
{
    ec_master_state_t ms;

    ecrt_master_state(master, &ms);

    if (ms.slaves_responding != master_state.slaves_responding)
        printf("%u slave(s).\n", ms.slaves_responding);
    if (ms.al_states != master_state.al_states)
        printf("AL states: 0x%02X.\n", ms.al_states);
    if (ms.link_up != master_state.link_up)
        printf("Link is %s.\n", ms.link_up ? "up" : "down");

    master_state = ms;
}

/*****************************************************************************/

void check_slave_config_states(void)
{
    ec_slave_config_state_t s;

    ecrt_slave_config_state(slave1_out1, &s);

    if (s.al_state != slave1_out1_state.al_state)
        printf("Slave1 Out1: State 0x%02X.\n", s.al_state);
    if (s.online != slave1_out1_state.online)
        printf("Slave1 Out1: %s.\n", s.online ? "online" : "offline");
    if (s.operational != slave1_out1_state.operational)
        printf("Slave1 Out1: %soperational.\n",
                s.operational ? "" : "Not ");

    slave1_out1_state = s;
}

/*****************************************************************************/

#if SDO_ACCESS
void read_sdo(void)
{
    switch (ecrt_sdo_request_state(sdo)) {
        case EC_REQUEST_UNUSED: // request was not used yet
            ecrt_sdo_request_read(sdo); // trigger first read
            break;
        case EC_REQUEST_BUSY:
            fprintf(stderr, "Still busy...\n");
            break;
        case EC_REQUEST_SUCCESS:
            fprintf(stderr, "SDO value: 0x%04X\n",
                    EC_READ_U16(ecrt_sdo_request_data(sdo)));
            ecrt_sdo_request_read(sdo); // trigger next read
            break;
        case EC_REQUEST_ERROR:
            fprintf(stderr, "Failed to read SDO!\n");
            ecrt_sdo_request_read(sdo); // retry reading
            break;
    }
}
#endif

/****************************************************************************/

void cyclic_task()
{
    int i;

    // receive process data
    ecrt_master_receive(master);
    ecrt_domain_process(domain1);

    // check process data state (optional)
    check_domain1_state();

    if (counter) {
        counter--;
    } else { // do this at 1 Hz
        counter = FREQUENCY;

        // calculate new process data
        blink = !blink;

        // check for master state (optional)
        check_master_state();

        // check for islave configuration state(s) (optional)
        check_slave_config_states();

#if SDO_ACCESS
        // read process data SDO
        read_sdo();
#endif

    }

#if 0
    // read process data
    printf("Slave1 Out1: state %u value %u\n",
            EC_READ_U8(domain1_pd + slave1_out1_status),
            EC_READ_U16(domain1_pd + slave1_out1_value));
#endif

#if 1
    // write process data
    EC_WRITE_U8(domain1_pd + off_slave1_out1, blink ? 0x01 : 0x00);
    //printf("XXX blink %d\n", blink);

#endif

    // send process data
    ecrt_domain_queue(domain1);
    ecrt_master_send(master);
}

/****************************************************************************/

void signal_handler(int signum) {
    switch (signum) {
        case SIGALRM:
            sig_alarms++;
            break;
    }
}

/****************************************************************************/

int main(int argc, char **argv)
{
    ec_slave_config_t *sc;
    struct sigaction sa;
    struct itimerval tv;
    
    master = ecrt_request_master(0);
    if (!master)
        return -1;

    domain1 = ecrt_master_create_domain(master);
    if (!domain1)
        return -1;

    if (!(slave1_out1 = ecrt_master_slave_config(
                    master, Slave1Pos, Beckhoff_EL2202))) {
        fprintf(stderr, "Failed to get slave configuration.\n");
        return -1;
    }

#if SDO_ACCESS
    fprintf(stderr, "Creating SDO requests...\n");
    if (!(sdo = ecrt_slave_config_create_sdo_request(slave1_out1, 
						     slave_1_pdo_entries[0].index, 
						     slave_1_pdo_entries[0].subindex, 
						     slave_1_pdo_entries[0].bitlength))) {
        fprintf(stderr, "Failed to create SDO request.\n");
        return -1;
    }
    ecrt_sdo_request_timeout(sdo, 500); // ms
#endif

#if CONFIGURE_PDOS
    printf("Configuring PDOs...\n");
    if (ecrt_slave_config_pdos(slave1_out1, EC_END, slave_1_syncs)) {
        fprintf(stderr, "Failed to configure PDOs.\n");
        return -1;
    }

    if (!(sc = ecrt_master_slave_config(
                    master, Slave1Pos, Beckhoff_EL2202))) {
        fprintf(stderr, "Failed to get slave configuration.\n");
        return -1;
    }

//    if (ecrt_slave_config_pdos(sc, EC_END, el4102_syncs)) {
//        fprintf(stderr, "Failed to configure PDOs.\n");
//        return -1;
//    }
//
//    if (!(sc = ecrt_master_slave_config(
//                    master, DigOutSlavePos, Beckhoff_EL2032))) {
//        fprintf(stderr, "Failed to get slave configuration.\n");
//        return -1;
//    }
//
//    if (ecrt_slave_config_pdos(sc, EC_END, el2004_syncs)) {
//        fprintf(stderr, "Failed to configure PDOs.\n");
//        return -1;
//    }
#endif

    // Create configuration for bus coupler
    sc = ecrt_master_slave_config(master, BusCouplerPos, Beckhoff_EK1100);
    if (!sc)
        return -1;

    if (ecrt_domain_reg_pdo_entry_list(domain1, domain1_regs)) {
        fprintf(stderr, "PDO entry registration failed!\n");
        return -1;
    }

    printf("Activating master...\n");
    if (ecrt_master_activate(master))
        return -1;

    if (!(domain1_pd = ecrt_domain_data(domain1))) {
        return -1;
    }

#if PRIORITY
    pid_t pid = getpid();
    if (setpriority(PRIO_PROCESS, pid, -19))
        fprintf(stderr, "Warning: Failed to set priority: %s\n",
                strerror(errno));
#endif

    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGALRM, &sa, 0)) {
        fprintf(stderr, "Failed to install signal handler!\n");
        return -1;
    }

    printf("Starting timer...\n");
    tv.it_interval.tv_sec = 0;
    tv.it_interval.tv_usec = 1000000 / FREQUENCY;
    tv.it_value.tv_sec = 0;
    tv.it_value.tv_usec = 1000;

    if (setitimer(ITIMER_REAL, &tv, NULL)) {
        fprintf(stderr, "Failed to start timer: %s\n", strerror(errno));
        return 1;
    }

    printf("Started.\n");
    while (1) {
        pause();

#if 0
        struct timeval t;
        gettimeofday(&t, NULL);
        printf("%u.%06u\n", t.tv_sec, t.tv_usec);
#endif

        while (sig_alarms != user_alarms) {
            cyclic_task();
            user_alarms++;
        }
    }

    return 0;
}

/****************************************************************************/
