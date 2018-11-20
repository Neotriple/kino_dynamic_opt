import numpy as np
from time import sleep


def display_motion(robot, q_traj, time):
    for i in range(len(time)):
        robot.display(q_traj[i])
        if i == 0:
            sleep(time[i])
        else:
            sleep(time[i] - time[i - 1])

def norm(vector, weights=None):
    def norm_(vector):
        if isinstance(vector, float):
          return np.sqrt(vector**2)
        elif len(vector) == 0:
          error("Vector of length zero supplied.")
        else:
          return np.sqrt((vector.transpose()).dot(vector).item(0))

    if isinstance(vector, list):
        if not weights is None:
            vector_evaluated = [norm_(vec(delta_t=0.01)) for vec in vector]
            vector = []
            for i in range(len(vector_evaluated)):
                if np.abs(weights[i]) > 1e-5:
                    vector.append(weights[i] * vector_evaluated[i])

            if not isinstance(vector, list):
                vector = [vector]
            return np.sum([norm_(vec) for vec in vector])
        else:
            return np.sum([norm_(vec(delta_t=0.01)) for vec in vector])
    else: 
        return norm_(vector)
