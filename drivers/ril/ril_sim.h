#ifndef RIL_SIM_H
#define RIL_SIM_H

int init_sim_hot_plug(struct device *target_device, struct workqueue_struct *queue);
void free_sim_hot_plug(void);
irqreturn_t sim_interrupt_handle(int irq, void *dev_id);

void ril_sim_notify_modem_reset(bool is_reseting);

#endif
