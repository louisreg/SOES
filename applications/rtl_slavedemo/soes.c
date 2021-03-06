/*
 * Licensed under the GNU General Public License version 2 with exceptions. See
 * LICENSE file in the project root for full license information
 */

 /** \file
 * \brief
 * The application.
 *
 * The application, the main loop that service EtherCAT.
 */

#include <kern.h>
#include <flash_drv.h>

#include <esc.h>
#include <esc_coe.h>
#include <esc_foe.h>
#include "utypes.h"
#include "config.h"
#include "bootstrap.h"

#define WD_RESET           1000
#define DEFAULTTXPDOMAP    0x1a00
#define DEFAULTRXPDOMAP    0x1600
#define DEFAULTTXPDOITEMS  1
#define DEFAULTRXPDOITEMS  1

uint32_t            encoder_scale;
uint32_t            encoder_scale_mirror;

/* Global variables used by the stack */
uint8_t     MBX[MBXBUFFERS * MAX(MBXSIZE,MBXSIZEBOOT)];
_MBXcontrol MBXcontrol[MBXBUFFERS];
_ESCvar     ESCvar;

/* Application variables */
_Rbuffer    Rb;
_Wbuffer    Wb;
_Cbuffer    Cb;

/* Private variables */
int               wd_cnt = WD_RESET;
volatile uint8_t  digoutput;
volatile uint8_t  diginput;
uint16_t          txpdomap = DEFAULTTXPDOMAP;
uint16_t          rxpdomap = DEFAULTRXPDOMAP;
uint8_t           txpdoitems = DEFAULTTXPDOITEMS;
uint8_t           rxpdoitems = DEFAULTTXPDOITEMS;


extern uint32_t local_boot_state;

/** Function to pre-qualify the incoming SDO download.
 *
 * @param[in] index      = index of SDO download request to check
 * @param[in] sub-index  = sub-index of SDO download request to check
 * @return 1 if the SDO Download is correct. 0 If not correct.
 */
int ESC_pre_objecthandler (uint16_t index, uint8_t subindex)
{
   if ((index == 0x1c12) && (subindex > 0) && (rxpdoitems != 0))
   {
      SDO_abort (index, subindex, ABORT_READONLY);
      return 0;
   }
   if ((index == 0x1c13) && (subindex > 0) && (txpdoitems != 0))
   {
      SDO_abort (index, subindex, ABORT_READONLY);
      return 0;
   }
   return 1;
}


/** Mandatory: Hook called from the slave stack SDO Download handler to act on
 * user specified Index and Sub-index.
 *
 * @param[in] index      = index of SDO download request to handle
 * @param[in] sub-index  = sub-index of SDO download request to handle
 */
void ESC_objecthandler (uint16_t index, uint8_t subindex)
{
   switch (index)
   {
      case 0x1c12:
      {
         if (rxpdoitems > 1)
         {
            rxpdoitems = 1;
         }
         if ((rxpdomap != 0x1600) && (rxpdomap != 0x1601)
             && (rxpdomap != 0x0000))
         {
            rxpdomap = 0x1600;
         }
         ESCvar.RXPDOsize = ESCvar.ESC_SM2_sml = sizeOfPDO(RX_PDO_OBJIDX);
         break;
      }
      case 0x1c13:
      {
         if (txpdoitems > 1)
         {
            txpdoitems = 1;
         }
         if ((txpdomap != 0x1A00) && (txpdomap != 0x1A01)
             && (rxpdomap != 0x0000))
         {
            txpdomap = 0x1A00;
         }
         ESCvar.TXPDOsize = ESCvar.ESC_SM3_sml = sizeOfPDO(TX_PDO_OBJIDX);
         break;
      }
      case 0x7100:
      {
         switch (subindex)
         {
            case 0x01:
            {
               encoder_scale_mirror = encoder_scale;
               break;
            }
         }
         break;
      }
      case 0x8001:
      {
         switch (subindex)
         {
            case 0x01:
            {
               Cb.reset_counter = 0;
               break;
            }
         }
         break;
      }
   }
}

/** Mandatory: Hook called from the slave stack ESC_stopoutputs to act on state changes
 * forcing us to stop outputs. Here we can set them to a safe state.
 * set
 */
void APP_safeoutput (void)
{
   DPRINT ("APP_safeoutput called\n");
   Wb.LED = 0;
}
/** Mandatory: Write local process data to Sync Manager 3, Master Inputs.
 */
void TXPDO_update (void)
{
   ESC_write (SM3_sma, &Rb.button, ESCvar.TXPDOsize);
}
/** Mandatory: Read Sync Manager 2 to local process data, Master Outputs.
 */
void RXPDO_update (void)
{
   ESC_read (SM2_sma, &Wb.LED, ESCvar.RXPDOsize);
}

/** Mandatory: Function to update local I/O, call read ethercat outputs, call
 * write ethercat inputs. Implement watch-dog counter to count-out if we have
 * made state change affecting the App.state.
 */
void DIG_process (void)
{
   if (wd_cnt)
   {
      wd_cnt--;
   }
   if (ESCvar.App.state & APPSTATE_OUTPUT)
   {
      /* SM2 trigger ? */
      if (ESCvar.ALevent & ESCREG_ALEVENT_SM2)
      {
         ESCvar.ALevent &= ~ESCREG_ALEVENT_SM2;
         RXPDO_update ();
         wd_cnt = WD_RESET;
         /* dummy output point */
         //gpio_set(GPIO_LED, Wb.LED & BIT(0));
      }
      if (!wd_cnt)
      {
         DPRINT("DIG_process watchdog tripped\n");
         ESC_stopoutput ();
         /* watchdog, invalid outputs */
         ESC_ALerror (ALERR_WATCHDOG);
         /* goto safe-op with error bit set */
         ESC_ALstatus (ESCsafeop | ESCerror);
      }
   }
   else
   {
      wd_cnt = WD_RESET;
   }
   if (ESCvar.App.state)
   {
      //Rb.button = gpio_get(GPIO_WAKEUP);
      Rb.button = (flash_drv_get_active_swap() && 0x8);
      Cb.reset_counter++;
      Rb.encoder =  Cb.reset_counter;
      TXPDO_update ();
   }
}

/** Optional: Hook called after state change for application specific
 * actions for specific state changes.
 */
void post_state_change_hook (uint8_t * as, uint8_t * an)
{

   /* Add specific step change hooks here */
   if ((*as == BOOT_TO_INIT) && (*an == ESCinit))
   {
      boot_inithook ();
   }
   else if((*as == INIT_TO_BOOT) && (*an & ESCerror ) == 0)
   {
      init_boothook ();
   }
}


/** SOES main loop. Start by initializing the stack software followed by
 * the application loop for cyclic read the EtherCAT state and staus, update
 * of I/O.
 */
void soes (void *arg)
{
   DPRINT ("SOES (Simple Open EtherCAT Slave)\n");

   ESCvar.TXPDOsize = ESCvar.ESC_SM3_sml = sizeOfPDO(TX_PDO_OBJIDX);
   ESCvar.RXPDOsize = ESCvar.ESC_SM2_sml = sizeOfPDO(RX_PDO_OBJIDX);

   /* Setup config hooks */
   static esc_cfg_t config =
   {
      .user_arg = "/spi0/et1100",
      .use_interrupt = 0,
      .watchdog_cnt = 0,
      .mbxsize = MBXSIZE,
      .mbxsizeboot = MBXSIZEBOOT,
      .mbxbuffers = MBXBUFFERS,
      .mb[0] = {MBX0_sma, MBX0_sml, MBX0_sme, MBX0_smc, 0},
      .mb[1] = {MBX1_sma, MBX1_sml, MBX1_sme, MBX1_smc, 0},
      .mb_boot[0] = {MBX0_sma_b, MBX0_sml_b, MBX0_sme_b, MBX0_smc_b, 0},
      .mb_boot[1] = {MBX1_sma_b, MBX1_sml_b, MBX1_sme_b, MBX1_smc_b, 0},
      .pdosm[0] = {SM2_sma, 0, 0, SM2_smc, SM2_act},
      .pdosm[1] = {SM3_sma, 0, 0, SM3_smc, SM3_act},      
      .pre_state_change_hook = NULL, 
      .post_state_change_hook = NULL,
      .application_hook = NULL,
      .safeoutput_override = NULL,
      .pre_object_download_hook = NULL,
      .post_object_download_hook = NULL,
      .rxpdo_override = NULL,
      .txpdo_override = NULL,
      .esc_hw_interrupt_enable = NULL,
      .esc_hw_interrupt_disable = NULL,
      .esc_hw_eep_handler = NULL
   };
   
   ESC_config (&config);
   ESC_init (&config);

   task_delay (tick_from_ms (200));

   /*  wait until ESC is started up */
   while ((ESCvar.DLstatus & 0x0001) == 0)
   {
      ESC_read (ESCREG_DLSTATUS, (void *) &ESCvar.DLstatus,
                sizeof (ESCvar.DLstatus));
      ESCvar.DLstatus = etohs (ESCvar.DLstatus);
   }

   /* Pre FoE to set up Application information */
   bootstrap_foe_init ();
   /* Init FoE */
   FOE_init();

   /* reset ESC to init state */
   ESC_ALstatus (ESCinit);
   ESC_ALerror (ALERR_NONE);
   ESC_stopmbx ();
   ESC_stopinput ();
   ESC_stopoutput ();

   DPRINT ("Application_loop GO\n");
   /* application run loop */
   while (1)
   {
      /* On init restore PDO mappings to default size */
      if((ESCvar.ALstatus & 0x0f) == ESCinit)
      {
         txpdomap = DEFAULTTXPDOMAP;
         rxpdomap = DEFAULTRXPDOMAP;
         txpdoitems = DEFAULTTXPDOITEMS;
         rxpdoitems = DEFAULTTXPDOITEMS;
      }
      /* Read local time from ESC*/
      ESC_read (ESCREG_LOCALTIME, (void *) &ESCvar.Time, sizeof (ESCvar.Time));
      ESCvar.Time = etohl (ESCvar.Time);

      /* Check the state machine */
      ESC_state ();

      /* If else to two separate execution paths
       * If we're running BOOSTRAP
       *  - MailBox
       *   - FoE
       * Else we're running normal execution
       *  - MailBox
       *   - CoE
       */
      if(local_boot_state)
      {
         if (ESC_mbxprocess ())
         {
            ESC_foeprocess ();
            ESC_xoeprocess ();
         }
         bootstrap_state ();
       }
      else
      {
         if (ESC_mbxprocess ())
         {
            ESC_coeprocess ();
            ESC_xoeprocess ();
         }
         DIG_process ();
      }
   };
}

uint8_t load1s, load5s, load10s;
void my_cyclic_callback (void * arg)
{
   while (1)
   {
      task_delay(tick_from_ms (20000));
      stats_get_load (&load1s, &load5s, &load10s);
      DPRINT ("%d:%d:%d (1s:5s:10s)\n",
               load1s, load5s, load10s);
      DPRINT ("Local bootstate: %d App.state: %d\n", local_boot_state,App.state);
      DPRINT ("AlStatus : 0x%x, AlError : 0x%x, Watchdog : %d \n", (ESCvar.ALstatus & 0x001f),ESCvar.ALerror,wd_cnt);

   }
}
