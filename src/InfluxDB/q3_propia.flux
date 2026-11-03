¿Cuantas lecturas de ruido en la ultima hora superaron la barrera de las 400 cuentas ADC?
from(bucket: "mediciones")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "mediciones")
  |> filter(fn: (r) => r.equipo == "E02" and r.proyecto == "P02" and r.nodo == "nodo1")
  |> filter(fn: (r) => r._field == "ky038_sonido")
  |> filter(fn: (r) => r._value > 400.0)
  |> count()