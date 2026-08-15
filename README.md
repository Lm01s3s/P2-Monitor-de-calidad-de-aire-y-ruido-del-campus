# P2 — Monitor de calidad de aire y ruido del campus

## E02 — Daniel Quintero
**Simulación Wokwi:** [E 02-GT1](https://wokwi.com/projects/472438716081998849)

### Integrantes
**Dylan Arias — Pablo Lazo — Lucas Maldonado — Sergio Mella — Gabriel Castro**

## 1. Lazo de Muestreo
> <!-- pablo borra esto y pone tu explicacion sin borrar el primer > -->

## 2. Calibración y Tolerancia
> Para corregir el error sistematico del sensor imperfecto, se obtuvieron 2 lecturas crudas y aplicamos la formula de correcion.
> 
> **Referencia (R) — Lectura medida (M)**  
>   R1 = 7 — M1 = 9.38  
>   R2 = 33 — M2 = 36.66  
>   **R3 = 19 — M3 = 21.97 (medido) , 19.02 (calibrado) , 18.98 (filtrado)**
>
> **Datos calculados y aplicados en el codigo**
> - Ganancia (m) = 0.953  
>   $m = \frac{R_2 - R_1}{M_2 - M_1}$
> 
> - Offset (b) = -1.939  
>   $b = R_1 - (m \times M_1)$
>
> Con esto pudimos verificar que tras inyectar los valores m y b calculados, al ingresar un nuevo punto (R3 = 19) se obtuvieron valores esperados con una tolerancia de ± 0.05 respecto al valor inicial.

## 3. Filtro y Ruido
> <!-- pablo borra esto y pone tu explicacion sin borrar el primer > -->
