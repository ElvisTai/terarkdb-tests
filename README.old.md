数据源目前只支持wikipedia的数据。如果要支持其他类型的数据，需要修改每个存储引擎对应的load、read、insert、update等函数。
如何运行可以参考shell文件夹下的各种脚本文件。
脚本主要分为两类，load和run。load代表的是预热阶段，run代表实际运行阶段。
脚本中可配置的参数：
--load_or_run=run（必须） 
--keys_data_path=$keys_file（必须） 
--db=$dirname （存储引擎存放数据的地址，必须）
--use_lsm=0 （非必须）
--insert_data_path=$insert_file（run模式必须，load模式不需要） 
--cache_size=$cachesize （非必须）
--read_percent=90 （非必须，默认90）
--insert_percent=5 （非必须，默认5）
--thread_num=8（非必须，默认。。。忘了）
--load_data_path=$load_file（load模式必须，run模式不需要）
以下是运行时可配置的参数,命令格式 -command=xxx:xxx:xxx,命令可以同时配置多个，但是之间的分割必须使用'\t'.
--stop=true|false //是否停止测试
--thread_num=线程个数
--sampling_rate=[0,100]//采样率，默认20%，
--compact=//启动compact，等号后不需要其他数据
--plan_config=plan_id:read_percent:update_percent:write_percent//读写计划设置，在线修改读写比只能通过这个命令，设置时必须设置读比例，更新比例，写比例，
三者的合法范围为[0,uint32_max],每一个执行计划都有一个ID，这个ID的作用在下一个命令介绍
-thread_plan_map=thread_id:plan_id//设置哪一个线程执行哪一个读写计划，线程id默认从0开始，假设系统有8个线程，线程ID分别是0到7，
新建的线程默认执行读写计划0。plan_id和thread_id必须已经存在，否则会设置失败
-message=//向存储引擎发送消息，消息格式是存储引擎自定义的。目前可以发送一个“-message=:”来探测引擎是否支持自定义消息。
如果支持，会在5S内发送回一个列表（必须手动发送一个非法命令，测试框架才会有回复），列表里就是可以修改的设置，具体格式和列表里的格式相同。
    