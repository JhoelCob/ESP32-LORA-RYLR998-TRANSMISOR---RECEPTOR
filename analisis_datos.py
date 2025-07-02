import statistics
import csv
import matplotlib.pyplot as plt

tiempo = 0 # Columna de  la  fecha y hora
var = 2 # Cambiar segun sea necesario

def extraer_datos_sensor(sensores):
  datos_columna_1 = []
  datos_columna_3 = []
  with open(sensores, 'r', newline='') as archivo_csv:
    lector_csv = csv.reader(archivo_csv)
    try:
      # Extraemos los nombres de las variables de la primera fila (encabezados)
      encabezados = next(lector_csv)
      nombre_col_1 = encabezados[tiempo]
      nombre_col_3 = encabezados[var]

    except (StopIteration, IndexError):
      # Si el archivo está vacío o no tiene suficientes columnas, usamos valores por defecto.
      return "Tiempo", [], "Valor", []

    for fila in lector_csv:
      try:
        # Extraemos el valor de la primera columna (índice 0)
        valor_col_1 = fila[tiempo]
        # Convertimos el valor de la tercera columna (índice 2) a float
        valor_col_3 = float(fila[var])
        # Añadimos los valores a sus respectivas listas
        datos_columna_1.append(valor_col_1)
        datos_columna_3.append(valor_col_3)
      except (ValueError, IndexError):
        # Si el valor de la col 3 no es un número o la fila no tiene suficientes columnas, la ignoramos.
        pass
  return nombre_col_1, datos_columna_1, nombre_col_3, datos_columna_3


def graficar_datos(tiempos, valores, etiqueta_x, etiqueta_y):
  plt.figure(figsize=(12, 6))  # Define un buen tamaño para la figura
  plt.plot(tiempos, valores, marker='.', linestyle='-', color='c', label=etiqueta_y)

  plt.title(f'Registro de {etiqueta_y} a lo largo de {etiqueta_x}', fontsize=16)
  plt.xlabel(etiqueta_x, fontsize=12)
  plt.ylabel(etiqueta_y, fontsize=12)

  # Mejora la legibilidad de las etiquetas del eje X
  plt.xticks(rotation=45, ha='right')

  # Evita la saturación del eje X mostrando un número limitado de etiquetas
  if len(tiempos) > 20:
    plt.gca().xaxis.set_major_locator(plt.MaxNLocator(20))

  plt.grid(True, which='both', linestyle='--', linewidth=0.5)
  plt.tight_layout()  # Ajusta el gráfico para que todo encaje correctamente
  plt.legend()
  plt.show()

sensores = 'sensors_2025-06-22.csv'
# Desempaquetamos los datos y los nombres de las columnas
nombre_col1, datos_col1, nombre_col3, datos_col3 = extraer_datos_sensor(sensores)

# Calculamos la desviación estándar y mostramos la gráfica
if len(datos_col3) > 1:
  desviacion_estandar_col3 = statistics.stdev(datos_col3)
  # print(f"Datos extraídos ({nombre_col1}): {datos_col1}") #Imprime los datos de la columna 1
  # print(f"Datos extraídos ({nombre_col3}): {datos_col3}") #Improme los datos de la columna 3
  print(f"La desviación estándar de '{nombre_col3}' es: {desviacion_estandar_col3:.2f}")
  # Llamada a la función para generar la gráfica con las etiquetas dinámicas
  graficar_datos(datos_col1, datos_col3, nombre_col1, nombre_col3)
else:
  print("No hay suficientes datos para calcular la desviación estándar o generar la gráfica.")

