import array
import statistics

def extraer_datos_sensor(sensores):

  # 'f' especifica que el array contendrá números de punto flotante (floats)
  datos_sensor = array.array('f')
  with open(sensores, 'r') as archivo_csv:
    lector_csv = csv.reader(archivo_csv)
    next(lector_csv)  # Omitir la primera fila (encabezados)
    for fila in lector_csv:
      try:
        # Convertimos el valor de la tercera columna a float y lo añadimos al array
        datos_sensor.append(float(fila[2]))
      except (ValueError, IndexError):
        # Si el valor no es un número o la fila no tiene suficientes columnas, la ignoramos.
        pass
  return datos_sensor

sensores = 'sensors_2025-06-22.csv'
datos_array = extraer_datos_sensor(sensores)

if len(datos_array) > 1:
  desviacion_estandar = statistics.stdev(datos_array)
  print(f"Datos extraídos (columna 2): {list(datos_array)}")
  print(f"La desviación estándar es: {desviacion_estandar}")
else:
  print("No hay suficientes datos para calcular la desviación estándar.")

