# ami-fep

1 connect

2 upstream
	register -> database {read write}
	heartbeat -> database {read write}
	data notification
	error -> database {read write}

3 downstream -> database {read}

4 disconnect -> database {read write}