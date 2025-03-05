import logging

def setup_logging():
    logger = logging.getLogger('d1')
    logger.setLevel(logging.INFO)

    c_handler = logging.StreamHandler()
    f_handler = logging.FileHandler('d1.log')

    c_format = logging.Formatter('%(asctime)s - %(message)s')
    f_format = logging.Formatter('%(asctime)s - %(message)s')

    c_handler.setFormatter(c_format)
    f_handler.setFormatter(f_format)

    logger.addHandler(c_handler)
    logger.addHandler(f_handler)

def logging_set_filename(filename):
    logger = logging.getLogger('d1')
    f_handler = logging.FileHandler(filename + ".log")
    f_format = logging.Formatter('%(asctime)s - %(message)s')
    f_handler.setFormatter(f_format)
    logger.addHandler(f_handler)
